# test_sr_drive_finalization.ps1 - the PRODUCTION ADAPTERS and FINALIZATION,
# driven through injected probes. Not source-text assertions.
#
# WHY IT EXISTS: the director rejected W7/W8 as source-text checks and named the
# three cases that must actually be exercised - key-down SendInput=0, key-up
# SendInput=0, and a cleanup key that stays down - each producing a final
# FAILURE, exact counts, NO "ALL STEPS PASSED", and retained stuck-key state.
# It also required Wait-SustainedBlack's frameDir/notBefore BINDINGS to be
# exercised, because that wrapper is where a wrong directory or a forgotten
# post-entry filter would hide unseen.
#
# Everything here runs offline against the same bodies production calls.
# Exit code = number of failed checks. ASCII only.

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)
. (Join-Path $PSScriptRoot 'sr_drive_core.ps1')

$pass = 0; $fail = 0
function Check([bool]$ok, [string]$name) {
    if ($ok) { $script:pass++; Write-Host "  ok  $name" }
    else { $script:fail++; Write-Host "FAIL: $name" }
}

# ── probe factory ───────────────────────────────────────────────────────────
# $downPlan: per-vk queue of IsDown answers. $sendOk: whether SendUp succeeds.
function New-RelProbes($downPlan, [bool]$sendOk) {
    $i = [ref]0; $sent = [ref]0; $notes = New-Object System.Collections.ArrayList
    @{
        SendUp = { param($vk, $ext) $sent.Value++; $sendOk }.GetNewClosure()
        IsDown = { param($vk)
                   $v = $downPlan[[Math]::Min($i.Value, $downPlan.Count - 1)]
                   $i.Value++; [bool]$v }.GetNewClosure()
        Sleep  = { param($ms) }.GetNewClosure()
        Note   = { param($m) [void]$notes.Add($m) }.GetNewClosure()
        _sent  = $sent; _notes = $notes
    }
}

# ── A. CLEANUP KEY REMAINING DOWN ───────────────────────────────────────────
$held = @(@{ vk = 0x26; ext = $true })
$p = New-RelProbes @($true) $true
$r = Invoke-ReleaseAllBody $held 5 $p
Check ($r.stuck -eq 1) "A1: a cleanup key that never clears counts exactly ONE stuck key"
Check ($r.remaining.Count -eq 1 -and $r.remaining[0].vk -eq 0x26) `
    "A2: the unreleased key is RETAINED, not discarded - the bookkeeping survives"
Check (($p._notes | Where-Object { $_ -like '*RETAINED*' }).Count -eq 1) `
    "A3: the retention is NAMED once"

$p = New-RelProbes @($false) $true
$r = Invoke-ReleaseAllBody @(@{ vk = 0x41; ext = $false }) 5 $p
Check ($r.stuck -eq 0 -and $r.remaining.Count -eq 0) `
    "A4: a key that releases cleanly is NOT retained and is NOT counted stuck"

# ── B. KEY-UP SendInput = 0, counted by the PROBE ──────────────────────────
# ⚠ The body deliberately does NOT tally send failures - the SendUp probe owns
# them, so there is exactly one source of truth. A second tally in the body is
# what double-counted every release failure once already.
$sends = [ref]0
$probes = @{
    SendUp = { param($vk,$ext) $sends.Value++; $false }.GetNewClosure()
    IsDown = { param($vk) $true }
    Sleep  = { param($ms) }
    Note   = { param($m) }
}
$r = Invoke-ReleaseAllBody @(@{ vk = 0x26; ext = $true }) 3 $probes
Check ($sends.Value -eq 4) `
    "B1: the PROBE sees every up-event (1 opening + 3 retries), got $($sends.Value)"
Check ($null -eq $r.sendFailures) `
    "B2: the body reports NO sendFailures of its own - one source of truth"
Check ($r.stuck -eq 1 -and $r.remaining.Count -eq 1) `
    "B3: a key whose up-events all failed is stuck AND retained"

# ── C. COMPOSED PRODUCTION BINDING ─────────────────────────────────────────
# ⛔ THE DEFECT THIS REPLACES. The previous section injected the desired counts
# straight into the finalizer, so it could not see that production captured the
# send-failure counter BEFORE cleanup while the cleanup adapter returned a
# hardcoded zero. A cleanup SendInput=0 therefore produced finalOk=true,
# finalSendFailures=0, globalAfterRelease=1 and "ALL STEPS PASSED".
#
# These COMPOSE the real shape instead: one canonical health record, mutated by
# the same SendUp probe production uses, read once inside the finalizer. The
# only fakes are the Win32 edges.
function New-ComposedRig([bool]$sendUpOk, $downPlan) {
    $health = @{ sendFailures = 0; stuckKeys = 0 }
    $notes  = New-Object System.Collections.ArrayList
    $i = [ref]0
    $probes = @{
        # mirrors production Send-Key: increments the canonical record ON FAILURE
        SendUp = { param($vk, $ext)
                   if (-not $sendUpOk) { $health.sendFailures++; return $false }
                   return $true }.GetNewClosure()
        IsDown = { param($vk)
                   $v = $downPlan[[Math]::Min($i.Value, $downPlan.Count - 1)]
                   $i.Value++; [bool]$v }.GetNewClosure()
        Sleep  = { param($ms) }.GetNewClosure()
        Note   = { param($m) [void]$notes.Add($m) }.GetNewClosure()
    }
    @{ health = $health; probes = $probes; notes = $notes }
}
function Assert-Failed($f, $rig, [string]$tag, [int]$expSend, [int]$expStuck, [int]$expHeld) {
    Check (-not $f.ok) "$tag ok=false"
    Check ($f.verdictLine -notmatch 'ALL STEPS PASSED') "$tag no pass marker"
    Check ($f.verdictLine -like 'FAILED:*') "$tag verdict line is a FAILURE"
    Check ($f.sendFailures -eq $expSend) "$tag sendFailures=$expSend (got $($f.sendFailures))"
    Check ($f.stuckKeys -eq $expStuck) "$tag stuckKeys=$expStuck (got $($f.stuckKeys))"
    Check (@($f.remaining).Count -eq $expHeld) "$tag retained held keys=$expHeld (got $(@($f.remaining).Count))"
    Check ($rig.health.sendFailures -eq $f.sendFailures -and $rig.health.stuckKeys -eq $f.stuckKeys) `
        "$tag the REPORTED counts match the canonical record exactly once"
}

# C1: CONTROL. A clean run must still pass, or every case below is vacuous.
$rig = New-ComposedRig $true @($false)
$f = Invoke-CleanupAndFinalize $true '' @(@{vk=0x41;ext=$false}) 5 $rig.health $rig.probes
Check ($f.ok -and $f.verdictLine -eq 'ALL STEPS PASSED' -and $f.sendFailures -eq 0 -and $f.stuckKeys -eq 0) `
    "C1: CONTROL - a clean run passes, announces once, and reports zero"

# C2: KEY-DOWN SendInput=0, THROUGH THE REAL ADAPTER.
# ⚠ Was `$rig.health.sendFailures++` - a hand-increment, which could not tell a
# broken Send-Key from a working one. Invoke-SendKeyResult IS Send-Key's whole
# non-Win32 half, so driving it with Inserted=0 exercises production's own
# decision and recording.
$rig = New-ComposedRig $true @($false)
$adapterNotes = New-Object System.Collections.ArrayList
$sk = Invoke-SendKeyResult 0 5 0x41 $false $rig.health { param($m) [void]$adapterNotes.Add($m) }
Check ($sk -eq $false) "C2a: the real Send-Key adapter returns FALSE when SendInput inserts 0"
Check ($rig.health.sendFailures -eq 1) "C2b: it records exactly ONE failure in the canonical record"
Check (($adapterNotes | Where-Object { $_ -like '*Win32 error 5*' }).Count -eq 1) `
    "C2c: it NAMES the Win32 error once"
$sk2 = Invoke-SendKeyResult 1 0 0x41 $false $rig.health { param($m) }
Check ($sk2 -eq $true -and $rig.health.sendFailures -eq 1) `
    "C2d: a SUCCESSFUL send returns true and records nothing - the adapter is not always-fail"
$f = Invoke-CleanupAndFinalize $true '' @(@{vk=0x41;ext=$false}) 5 $rig.health $rig.probes
Assert-Failed $f $rig 'C2e (key-down SendInput=0 reaches the verdict):' 1 0 0

# C3: CLEANUP KEY-UP SendInput=0 WITH THE KEY READING UP.
# ⛔ THE EXACT BUG. The key releases fine, so nothing is stuck - the ONLY signal
# is the send failure raised DURING cleanup. A pre-capture cannot see it.
$rig = New-ComposedRig $false @($false)
$f = Invoke-CleanupAndFinalize $true '' @(@{vk=0x26;ext=$true}) 5 $rig.health $rig.probes
Assert-Failed $f $rig 'C4 (cleanup key-up SendInput=0, key reads UP):' 1 0 0

# C4: CLEANUP KEY REMAINING DOWN
$rig = New-ComposedRig $true @($true)
$f = Invoke-CleanupAndFinalize $true '' @(@{vk=0x26;ext=$true}) 5 $rig.health $rig.probes
Assert-Failed $f $rig 'C5 (cleanup key remains DOWN):' 0 1 1

# C5: ORDER - health reported before the verdict, verdict last
$rig = New-ComposedRig $true @($true)
$f = Invoke-CleanupAndFinalize $true '' @(@{vk=0x26;ext=$true}) 5 $rig.health $rig.probes
$ih = [array]::FindIndex([string[]]@($rig.notes), [Predicate[string]]{ param($m) $m -like 'input health:*' })
$iv = [array]::FindIndex([string[]]@($rig.notes), [Predicate[string]]{ param($m) $m -like 'FAILED:*' -or $m -eq 'ALL STEPS PASSED' })
Check ($ih -ge 0 -and $iv -gt $ih -and $iv -eq ($rig.notes.Count - 1)) `
    "C6: health is reported BEFORE the verdict and the verdict is the LAST line"

# C6: an already-failed run keeps its ORIGINAL reason
$rig = New-ComposedRig $true @($false)
$f = Invoke-CleanupAndFinalize $false 'the original failure' @() 5 $rig.health $rig.probes
Check ((-not $f.ok) -and $f.failReason -eq 'the original failure') `
    "C7: a run that already failed keeps its own reason"

# C7: EXACTLY ONCE - two keys failing to send must count two, not four
$rig = New-ComposedRig $false @($false)
$f = Invoke-CleanupAndFinalize $true '' @(@{vk=0x41;ext=$false}, @{vk=0x42;ext=$false}) 5 $rig.health $rig.probes
Check ($f.sendFailures -eq 2) `
    "C8: two cleanup send failures count TWO, not four - no double tally (got $($f.sendFailures))"

# ── CE. OUTCOME / EXIT MAPPING ─────────────────────────────────────────────
# Named in the director's bar and previously untested: the finalizer's boolean
# must reach BOTH the persisted outcome record and the process exit code. A run
# that fails and exits 0 would be read by any caller as a success.
$o = Resolve-RunOutcome @{ ok = $true; failReason = '' }
Check ($o.ok -eq $true -and $o.exitCode -eq 0 -and $o.result -eq 'SUCCESS') `
    "CE1: a passing run maps to exitCode 0 / SUCCESS"
$o = Resolve-RunOutcome @{ ok = $false; failReason = 'because' }
Check ($o.ok -eq $false -and $o.exitCode -eq 1 -and $o.result -eq 'FAILURE' -and $o.failReason -eq 'because') `
    "CE2: a failing run maps to exitCode 1 / FAILURE and carries its reason"

# CE3: the WHOLE chain - a cleanup send failure must reach a NONZERO exit.
$rig = New-ComposedRig $false @($false)
$f = Invoke-CleanupAndFinalize $true '' @(@{vk=0x26;ext=$true}) 5 $rig.health $rig.probes
$o = Resolve-RunOutcome $f
Check ($o.exitCode -eq 1 -and $o.result -eq 'FAILURE') `
    "CE3: a cleanup-only SendInput failure reaches a NONZERO process exit"

# CE4: stuckKeys is DERIVED, so one physical key cannot be reported as two even
# when a per-step release already failed for it.
$rig = New-ComposedRig $true @($true)
$rig.health.stuckKeys = 1            # as if a per-step release had already failed
$f = Invoke-CleanupAndFinalize $true '' @(@{vk=0x26;ext=$true}) 5 $rig.health $rig.probes
Check ($f.stuckKeys -eq 1 -and @($f.remaining).Count -eq 1) `
    "CE4: ONE retained physical key reports stuckKeys=1, not 2 - derived, never accumulated"

# CE5: a key rescued during cleanup is NOT stuck at the end
$rig = New-ComposedRig $true @($false)
$rig.health.stuckKeys = 1
$f = Invoke-CleanupAndFinalize $true '' @(@{vk=0x26;ext=$true}) 5 $rig.health $rig.probes
Check ($f.stuckKeys -eq 0 -and $f.ok) `
    "CE5: a key rescued during cleanup ends NOT stuck, and the run is healthy"

# ── D. Wait-SustainedBlack BINDINGS: frameDir and notBefore ─────────────────
$dir = Join-Path $env:TEMP ("srfin_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $dir | Out-Null
try {
    $old = Join-Path $dir 'old.bmp'; Set-Content $old 'x'
    (Get-Item $old).LastWriteTime = (Get-Date).AddMinutes(-10)
    $cut = (Get-Date).AddMinutes(-5)
    $seen = New-Object System.Collections.ArrayList
    $probes = New-BlackGateProbes $dir $cut `
        { param($path) [void]$seen.Add($path); 1 } `
        { 0 } { param($ms) } { param($m) }
    Check ($null -eq (& $probes.Newest)) `
        "D1: a frame written BEFORE notBefore is filtered out - the post-entry rule is in the binding"
    Check ($seen.Count -eq 0) "D2: the sampler is not even called for a pre-entry frame"

    $new = Join-Path $dir 'new.bmp'; Set-Content $new 'y'
    $f = & $probes.Newest
    Check ($null -ne $f -and $f.name -eq 'new.bmp') "D3: a POST-entry frame is returned"
    Check ($seen.Count -eq 1 -and $seen[0] -eq (Resolve-Path $new).Path) `
        "D4: the sampler is called with the frame's OWN path, from the bound frameDir"

    $other = Join-Path $env:TEMP ("srfin_other_" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force $other | Out-Null
    Set-Content (Join-Path $other 'decoy.bmp') 'z'
    Check ((& $probes.Newest).name -eq 'new.bmp') `
        "D5: a frame in a DIFFERENT directory is never returned - frameDir is really bound"
    Remove-Item $other -Recurse -Force -ErrorAction SilentlyContinue
} finally { Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue }

Write-Host "[FINAL_TEST] $pass/$($pass + $fail) passed"
exit $fail
