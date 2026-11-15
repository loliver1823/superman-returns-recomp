# iter-455l scenario-hardening core, r3 (director's four-closure-defect rework
# of r2). Functions ONLY - dot-sourced by tools/sr_drive.ps1 (real probes) and
# tools/test_sr_drive_hardened.ps1 (simulated probes). No top-level code.
# ASCII only (cp1252 parse hazard, see sr_drive.ps1 header).
#
# r3 contract (r2's guarantees plus the four ruled closures):
#   1. Focus cannot smuggle input past the deadline: after Focus returns, the
#      body re-runs the full watch (alive/expiry/transition) BEFORE any
#      key-down; a focus FAILURE rechecks liveness first so process/window
#      death during focus carries its real reason.
#   2. Transition detection is immediate: every engine wait (hold, confirm,
#      gap, out-of-pulses poll) runs through ONE watch primitive that checks
#      alive -> expiry -> runtime transition -> confirm each 100 ms quantum.
#      A hit while the key is down returns through finally (key released).
#   3. Liveness order is process-exit FIRST (Get-SrAliveReason, shared by
#      production and the sim): when an exit takes its HWND with it, the
#      reason is 'process exited', never 'window vanished'.
#   4. The validator pins the exact shipped contract (names, key, expectMs,
#      maxPulses, holdMs, gapMs, regexes, booleans) - not ranges.

# The exact shipped contract. -ceq / -eq comparisons: any deviation is a
# validation failure, not a silent behavior change.
$script:SR_HARD_SELECTOR = 'XamShowDeviceSelectorUI\(user='
$script:SR_HARD_HINTB    = '\[APT_REQ\] #\d+ path=''\.\./common/ButtonHints/XBox360/HintB'''
$script:SR_HARD_CONTRACT = @{
    awaitName = 'await-ui-tail'
    settleName = 'settle-dialog'
    clearName = 'clear-dialog'
    key = 'A'
    expectMs = 150000
    maxPulses = 12
    holdMs = 4000
    gapMs = 5000
    awaitTimeoutMs = 180000
    settleDwellMs = 8000
}

# The runtime selector invocation - never the import/banner text (which
# carries the bare name without "(user="). PRODUCTION PATH: the watch refuses
# to declare success unless the matched log line passes this predicate.
function Test-RuntimeSelectorLine([string]$line) {
    return [bool]($line -match 'XamShowDeviceSelectorUI\(user=')
}

# Liveness with the causally-correct order: a process exit usually destroys
# its HWND in the same instant, so the window check must come SECOND or the
# real cause is masked as 'window vanished'. Shared by the production Alive
# probe and the test sim (director rework item 3).
function Get-SrAliveReason([bool]$HasExited, $ExitCode, [bool]$WindowOk, $Hwnd) {
    if ($HasExited) { return ("process exited (code {0})" -f $ExitCode) }
    if (-not $WindowOk) { return ("window vanished (HWND {0})" -f $Hwnd) }
    return $null
}

# Strict validator: the scenario must carry the EXACT shipped contract.
# Returns @{ ok; reason; awaitName; settleName; clearName; awaitIdx;
# settleIdx; clearIdx }; ok=$false names the first violated requirement.
function Assert-HardenedScenario($scenario) {
    function fail([string]$why) { return @{ ok = $false; reason = $why } }
    $ctr = $script:SR_HARD_CONTRACT
    $steps = @($scenario.steps)
    $hard = @()
    for ($i = 0; $i -lt $steps.Count; $i++) {
        if ($steps[$i].singleDeadline) { $hard += $i }
    }
    if ($hard.Count -eq 0) { return fail "no singleDeadline step - the hardened contract requires the named '$($ctr.clearName)' step with singleDeadline=true" }
    if ($hard.Count -gt 1) { return fail "more than one singleDeadline step ($($hard.Count)) - the contract covers exactly one" }
    $ci = $hard[0]; $c = $steps[$ci]
    if ([string]$c.name -cne [string]$ctr.clearName) { return fail "the singleDeadline step is named '$($c.name)', not '$($ctr.clearName)'" }
    if ([string]$c.key -cne [string]$ctr.key) { return fail "'$($ctr.clearName)' key is '$($c.key)' - the pinned contract key is '$($ctr.key)'" }
    if (-not [bool]$c.holdUntilConfirmed) { return fail "'$($ctr.clearName)' does not set holdUntilConfirmed=true" }
    if ([string]$c.expect -cne $script:SR_HARD_SELECTOR) { return fail "'$($ctr.clearName)' expect is '$($c.expect)' - the contract requires the exact runtime-only selector expression '$script:SR_HARD_SELECTOR'" }
    if ([int]$c.expectMs -ne [int]$ctr.expectMs) { return fail "'$($ctr.clearName)' expectMs=$($c.expectMs) - the pinned contract value is $($ctr.expectMs)" }
    if ([int]$c.maxPulses -ne [int]$ctr.maxPulses) { return fail "'$($ctr.clearName)' maxPulses=$($c.maxPulses) - the pinned contract value is $($ctr.maxPulses)" }
    if ([int]$c.holdMs -ne [int]$ctr.holdMs) { return fail "'$($ctr.clearName)' holdMs=$($c.holdMs) - the pinned contract value is $($ctr.holdMs)" }
    if ([int]$c.gapMs -ne [int]$ctr.gapMs) { return fail "'$($ctr.clearName)' gapMs=$($c.gapMs) - the pinned contract value is $($ctr.gapMs)" }
    $ai = -1
    for ($i = 0; $i -lt $ci; $i++) {
        if ([string]$steps[$i].name -ceq [string]$ctr.awaitName) { $ai = $i; break }
    }
    if ($ai -lt 0) { return fail "no step named '$($ctr.awaitName)' before '$($ctr.clearName)' - readiness is not proven before pulses" }
    $a = $steps[$ai]
    if (-not $a.await -or ([string]$a.await -cne $script:SR_HARD_HINTB)) { return fail "'$($ctr.awaitName)' await is not the exact named HintB regex" }
    if ([int]$a.timeoutMs -ne [int]$ctr.awaitTimeoutMs) { return fail "'$($ctr.awaitName)' timeoutMs=$($a.timeoutMs) - the pinned contract value is $($ctr.awaitTimeoutMs)" }
    $si = -1
    for ($i = $ai + 1; $i -lt $ci; $i++) {
        if ([string]$steps[$i].name -ceq [string]$ctr.settleName) { $si = $i; break }
    }
    if ($si -lt 0) { return fail "no step named '$($ctr.settleName)' between '$($ctr.awaitName)' and '$($ctr.clearName)'" }
    $s = $steps[$si]
    if ([int]$s.dwellMs -ne [int]$ctr.settleDwellMs) { return fail "'$($ctr.settleName)' dwellMs=$($s.dwellMs) - the pinned contract value is $($ctr.settleDwellMs)" }
    return @{ ok = $true; reason = 'ok'
              awaitName = [string]$a.name; settleName = [string]$s.name; clearName = [string]$c.name
              awaitIdx = $ai; settleIdx = $si; clearIdx = $ci }
}

# THE shared hardened-dialog body: readiness wait -> bounded settle -> pulse
# engine under one absolute deadline. Called by sr_drive.ps1 with real probes
# and by the test with simulated ones - there is no other implementation.
#
#   $AwaitStep/$SettleStep/$ClearStep : the SHIPPED scenario step objects
#   $ConfirmPat : delivery-confirmation regex (guest sampled the key), or ''
#   $P : probes @{
#       NowMs   = {}            -> int64 ms (monotonic enough for one step)
#       CountOf = { param($pat) } -> hits of $pat in the log (pumps first)
#       HitOf   = { param($pat) } -> the LAST matching line for $pat (or $null)
#       Alive   = {}            -> $null when healthy, else the fatal reason
#                                  string (use Get-SrAliveReason: exit first)
#       Focus   = {}            -> $true iff the game window took focus
#       KeyDown = {} / KeyUp = {} -> the clear key, down/up
#       Sleep   = { param($ms) } -> sleep (real: pumps the log while waiting)
#       Note    = { param($m) }  -> journal line
#   }
# Returns @{ ok; reason; fatal; phase; delivered; pulses; elapsedMs }.
# fatal=$true means a probe reported focus/window/process death - the caller
# must fail the step immediately with `reason`, never retry it.
function Invoke-HardenedDialog($AwaitStep, $SettleStep, $ClearStep, $ConfirmPat, $P) {
    $start = & $P.NowMs
    function done([bool]$ok, [string]$reason, [bool]$fatal, [string]$phase, [int]$delivered, [int]$pulses) {
        return @{ ok = $ok; reason = $reason; fatal = $fatal; phase = $phase
                  delivered = $delivered; pulses = $pulses
                  elapsedMs = ((& $P.NowMs) - $start) }
    }
    # Alive-aware sleep for the pre-engine phases (readiness/settle): <=250 ms
    # chunks so window/process death interrupts a wait instead of being
    # discovered at a deadline. Returns $null, or the fatal reason.
    $sleepAlive = {
        param($ms)
        $sdl = (& $P.NowMs) + [int64]$ms
        while ($true) {
            $a = & $P.Alive
            if ($a) { return [string]$a }
            $rem = $sdl - (& $P.NowMs)
            if ($rem -le 0) { return $null }
            & $P.Sleep ([Math]::Min(250, $rem))
        }
    }

    # -- phase 1: READINESS. Zero key input exists before this loop exits. ---
    # The log sample may itself pump and consume time (production CountOf
    # runs Pump-Log), so liveness and expiry are re-applied AFTER it, before
    # the sampled result is interpreted: HintB first observed at-or-after the
    # readiness deadline does NOT count (r4 defect 2).
    $rdl = (& $P.NowMs) + [int64]$AwaitStep.timeoutMs
    & $P.Note ("hardened: READINESS await /{0}/ (<= {1} ms)" -f $AwaitStep.await, [int]$AwaitStep.timeoutMs)
    while ($true) {
        $a = & $P.Alive
        if ($a) { return (done $false $a $true 'readiness' 0 0) }
        $cnt = & $P.CountOf $AwaitStep.await
        $a = & $P.Alive
        if ($a) { return (done $false $a $true 'readiness' 0 0) }
        if ((& $P.NowMs) -ge $rdl) {
            return (done $false 'readiness never observed within its deadline - no input was sent' $false 'readiness' 0 0)
        }
        if ($cnt -gt 0) { break }
        $rem = $rdl - (& $P.NowMs)
        $f = & $sleepAlive ([Math]::Min(500, $rem))
        if ($f) { return (done $false $f $true 'readiness' 0 0) }
    }
    & $P.Note 'hardened: readiness observed'

    # -- phase 2: BOUNDED SETTLE. ------------------------------------------
    & $P.Note ("hardened: settle {0} ms" -f [int]$SettleStep.dwellMs)
    $f = & $sleepAlive ([int64]$SettleStep.dwellMs)
    if ($f) { return (done $false $f $true 'settle' 0 0) }

    # -- phase 3: PULSE ENGINE, one ABSOLUTE deadline. ---------------------
    $ebase     = & $P.NowMs
    $deadline  = $ebase + [int64]$ClearStep.expectMs
    $maxPulses = [int]$ClearStep.maxPulses
    $holdMs    = [int64]$ClearStep.holdMs
    $gapMs     = [int64]$ClearStep.gapMs
    $confirmMs = [int64]$(if ($ClearStep.confirmMs) { [int]$ClearStep.confirmMs } else { 6000 })
    $delivered = 0
    $pulses    = 0
    $wstate    = @{ refusedNoted = $false }

    # THE single wait primitive (rework item 2): every engine wait goes
    # through here. Per 100 ms quantum, in this order:
    #   alive -> absolute-deadline expiry -> runtime transition (line must
    #   pass Test-RuntimeSelectorLine) -> optional delivery confirm -> until.
    # Expiry before transition: a transition first observed at-or-after
    # expiry never turns the expired step into success. Returns
    # @{ kind = 'fatal'|'expired'|'transition'|'confirm'|'timeout'; reason }.
    $watch = {
        param([int64]$untilMs, [bool]$checkConfirm, [int]$mark)
        while ($true) {
            $a = & $P.Alive
            if ($a) { return @{ kind = 'fatal'; reason = [string]$a } }
            if ((& $P.NowMs) -ge $deadline) { return @{ kind = 'expired' } }
            # The log sample may itself pump and consume time (production
            # CountOf runs Pump-Log), so liveness and expiry are re-applied
            # AFTER it, before the sampled result is interpreted (r4 defect
            # 2): a selector first observed at-or-after the deadline is
            # expiry, never success.
            $cnt = & $P.CountOf $ClearStep.expect
            $a = & $P.Alive
            if ($a) { return @{ kind = 'fatal'; reason = [string]$a } }
            if ((& $P.NowMs) -ge $deadline) { return @{ kind = 'expired' } }   # post-sample expiry: the pump may have crossed the deadline
            if ($cnt -gt 0) {
                $line = & $P.HitOf $ClearStep.expect
                if ($line -and (Test-RuntimeSelectorLine ([string]$line))) { return @{ kind = 'transition' } }
                if (-not $wstate.refusedNoted) {
                    $wstate.refusedNoted = $true
                    & $P.Note ("hardened: /{0}/ matched a NON-runtime line, refusing it: {1}" -f $ClearStep.expect, $line)
                }
            }
            if ($checkConfirm -and $ConfirmPat) {
                $ccnt = & $P.CountOf $ConfirmPat
                $a = & $P.Alive
                if ($a) { return @{ kind = 'fatal'; reason = [string]$a } }
                if ((& $P.NowMs) -ge $deadline) { return @{ kind = 'expired' } }   # post-sample expiry (confirm site): a confirm crossing the deadline counts nothing
                if ($ccnt -gt $mark) { return @{ kind = 'confirm' } }
            }
            $now = & $P.NowMs
            if ($now -ge $untilMs) { return @{ kind = 'timeout' } }
            & $P.Sleep ([Math]::Min(100, [Math]::Min($untilMs, $deadline) - $now))
        }
    }
    # Maps a terminal watch result to the body's return value; $null = go on.
    function wmap($w, [string]$phase) {
        switch ([string]$w.kind) {
            'fatal'      { return (done $false ([string]$w.reason) $true $phase $delivered $pulses) }
            'expired'    { return (done $false 'single-deadline expired' $false $phase $delivered $pulses) }
            'transition' { return (done $true 'observed' $false $phase $delivered $pulses) }
        }
        return $null
    }

    & $P.Note ("hardened: engine - ONE deadline {0} ms, up to {1} pulse(s)" -f [int]$ClearStep.expectMs, $maxPulses)
    while ($true) {
        $w = & $watch (& $P.NowMs) $false 0
        $r = wmap $w 'engine'; if ($r) { return $r }
        if ($pulses -lt $maxPulses) {
            $pulses++
            # The post-focus watch runs REGARDLESS of the focus result (r4
            # defect 3): alive -> expiry -> transition are all senior to a
            # focus-failure report, so death during focus carries its real
            # reason, a focus that consumed the budget is expiry (with no
            # key-down), and a selector that appeared during a failed focus
            # is recognized as success. Only then is focus failure reported.
            $fOk = & $P.Focus
            $w = & $watch (& $P.NowMs) $false 0   # post-focus watch: runs for BOTH focus outcomes - no key and no focus-failure report after a terminal result
            $r = wmap $w 'engine'; if ($r) { return $r }
            if (-not $fOk) {
                return (done $false 'could not focus the game window' $true 'engine' $delivered $pulses)
            }
            $mark = $(if ($ConfirmPat) { & $P.CountOf $ConfirmPat } else { 0 })
            # The confirmation BASELINE is the FOURTH time-consuming pumping
            # sample (r5 blocker): its pump can also cross the deadline,
            # expose the selector, or outlive the process - so the terminal
            # ordering runs once more and no key follows a terminal result.
            $w = & $watch (& $P.NowMs) $false 0   # post-mark watch: no key-down after a terminal result (r5)
            $r = wmap $w 'engine'; if ($r) { return $r }
            $conf = $false
            $term = $null
            & $P.KeyDown
            try {
                # Hold-until-confirmed; the watch enforces the absolute
                # deadline per quantum, the Min is belt-and-braces. A
                # transition or fatal DURING the hold exits through finally,
                # releasing the key (rework item 2).
                $w = & $watch ([Math]::Min((& $P.NowMs) + $holdMs, $deadline)) $true $mark
                if ($w.kind -eq 'confirm') { $conf = $true }
                elseif ($w.kind -ne 'timeout') { $term = $w }
            } finally {
                & $P.KeyUp    # key-up ALWAYS
            }
            if ($term) { $r = wmap $term 'engine'; if ($r) { return $r } }
            if (-not $conf -and $ConfirmPat) {
                # Post-release confirmation wait, same watch.
                $w = & $watch ([Math]::Min((& $P.NowMs) + $confirmMs, $deadline)) $true $mark
                if ($w.kind -eq 'confirm') { $conf = $true }
                elseif ($w.kind -ne 'timeout') { $r = wmap $w 'engine'; if ($r) { return $r } }
            }
            if ($conf -or -not $ConfirmPat) { $delivered++ }
            & $P.Note ("hardened: pulse {0}/{1} {2}" -f $pulses, $maxPulses, $(if ($conf -or -not $ConfirmPat) { 'DELIVERED' } else { 'NOT DELIVERED' }))
            if ($pulses -lt $maxPulses) {
                $w = & $watch ([Math]::Min((& $P.NowMs) + $gapMs, $deadline)) $false 0
                $r = wmap $w 'engine'; if ($r) { return $r }
            }
            continue
        }
        # Out of pulses: watch in 1 s slices until the deadline.
        $w = & $watch ([Math]::Min((& $P.NowMs) + 1000, $deadline)) $false 0
        $r = wmap $w 'engine'; if ($r) { return $r }
    }
}

# ── iter-456q: THE SUSTAINED-TERMINAL-BLACK GATE ────────────────────────────
# Required by the director after iter-456q spent its 240 s measurement dwell in
# the FRONTEND while every step reported healthy. A measurement must not begin
# until the era under test has demonstrably been entered.
#
# TWO RULES, both paid for by that invalid acquisition:
#   1. POST-STEP-ENTRY ONLY. Frames from before the step began do not count.
#      Terminal black is NOT a unique signature - the same black frame occurred
#      NINE times during boot in that run - so a gate keyed on "black has been
#      seen" would have passed during the intro movies.
#   2. SUSTAINED, NOT MATCHED. Explicitly NOT a cumulative black-hash match: the
#      gate needs a CONTINUOUS RUN spanning the required duration, and one
#      non-black frame resets the run.

# Pure: is this frame FULL-FRAME EXACT BLACK? Returns 1 yes, 0 no, -1 unreadable.
#
# ⛔ NOT A SAMPLE, AND NOT A THRESHOLD. The first cut sampled 256 of 921,600
# pixels and accepted any channel <= 4, which proves neither "full-frame" nor
# "exact black" - the director rejected it for exactly that. Terminal black in
# this engine is EXACT: a known loadscr4 terminal frame is the four bytes
# 00 00 00 FF repeated 921,600 times, every B/G/R byte zero.
#
# So the fast path is one .NET SequenceEqual against that precomputed pattern -
# full frame, exact, no PowerShell loop. If it misses, the frame is either not
# black or black with a different alpha, and the fallback distinguishes those by
# scanning B/G/R with an early exit. A rendered scene exits within a few pixels;
# only a genuinely-black-but-odd-alpha frame pays for the full scan.
function Test-FrameExactBlack([string]$path) {
    try { $bytes = [System.IO.File]::ReadAllBytes($path) } catch { return -1 }
    if ($bytes.Length -lt 54) { return -1 }
    if ($bytes[0] -ne 0x42 -or $bytes[1] -ne 0x4D) { return -1 }
    $off = [BitConverter]::ToInt32($bytes, 10)
    $w   = [BitConverter]::ToInt32($bytes, 18)
    $h   = [BitConverter]::ToInt32($bytes, 22)
    $bpp = [BitConverter]::ToInt16($bytes, 28)
    if ($bpp -ne 32 -or $w -le 0) { return -1 }
    $rows = [Math]::Abs($h)
    $need = [int64]$w * $rows * 4
    if ($need -le 0 -or ($off + $need) -gt $bytes.Length) { return -1 }

    $px = New-Object byte[] $need
    [Array]::Copy($bytes, $off, $px, 0, $need)

    # fast path: the whole pixel array equals 00 00 00 FF repeated
    if ($null -eq $script:SrBlackPattern -or $script:SrBlackPattern.Length -ne $need) {
        $pat = New-Object byte[] $need
        for ($i = 3; $i -lt $need; $i += 4) { $pat[$i] = 255 }
        $script:SrBlackPattern = $pat
    }
    if ([System.Linq.Enumerable]::SequenceEqual([byte[]]$px, [byte[]]$script:SrBlackPattern)) {
        return 1
    }
    # fallback: exact on B/G/R, alpha ignored. Early-exits on any lit pixel.
    for ($i = 0; $i -lt $need; $i += 4) {
        if ($px[$i] -ne 0 -or $px[$i+1] -ne 0 -or $px[$i+2] -ne 0) { return 0 }
    }
    return 1
}

# Injectable body. $P = @{ NowMs; Sleep; Note; Newest }, where Newest returns
# $null or @{ id; name; timeMs; black } for the newest POST-ENTRY frame - the
# caller owns the post-entry filter, so this body cannot forget it, and `black`
# is the TRI-STATE from Test-FrameExactBlack (1 / 0 / -1) rather than a value to
# be thresholded here.
# Returns @{ ok; reason; framesSeen; everBlack; spanMs }.
function Invoke-SustainedBlackGate([int]$RequireMs, [int]$TimeoutMs, $P) {
    $deadline  = (& $P.NowMs) + [int64]$TimeoutMs
    $runStart  = $null
    $lastId    = $null
    $everBlack = $false
    $framesSeen = 0
    $span      = 0
    while ((& $P.NowMs) -lt $deadline) {
        $f = & $P.Newest
        if ($f -and $f.id -ne $lastId) {
            $lastId = $f.id
            $framesSeen++
            if ([int]$f.black -lt 0) {
                # Unreadable, almost always a half-written frame. It is neither
                # black nor not-black, so it must NOT extend a run and must not
                # break one either.
            } elseif ([int]$f.black -eq 1) {
                $everBlack = $true
                if ($null -eq $runStart) {
                    $runStart = [int64]$f.timeMs
                    & $P.Note ("black-gate: run STARTED at {0} (full-frame exact black)" -f $f.name)
                }
                $span = [int64]$f.timeMs - $runStart
                if ($span -ge $RequireMs) {
                    & $P.Note ("black-gate: SUSTAINED {0} ms of post-entry terminal black over {1} frames" -f $span, $framesSeen)
                    return @{ ok = $true; reason = 'sustained'; framesSeen = $framesSeen
                              everBlack = $true; spanMs = $span }
                }
            } else {
                if ($null -ne $runStart) {
                    & $P.Note ("black-gate: run BROKEN by {0} (not full-frame exact black) - restarting" -f $f.name)
                }
                $runStart = $null
                $span = 0
            }
        }
        & $P.Sleep 500
    }
    if (-not $everBlack) {
        $reason = 'never-black'
        & $P.Note ("black-gate: FAILED - no post-entry frame was black within {0} ms ({1} frames). The era under test was NEVER ENTERED." -f $TimeoutMs, $framesSeen)
    } else {
        $reason = 'not-sustained'
        & $P.Note ("black-gate: FAILED - black seen but never sustained for {0} ms within {1} ms ({2} frames)." -f $RequireMs, $TimeoutMs, $framesSeen)
    }
    return @{ ok = $false; reason = $reason; framesSeen = $framesSeen
              everBlack = $everBlack; spanMs = $span }
}

# ── iter-458: THE SUSTAINED-VISIBLE GATE (Astra 038) ────────────────────────
# The mirror of the black gate, for "the title is captured VISIBLE before any
# input". A log marker alone is not enough: in iter458_hrr2 the title's UI tail
# (APT_REQ #15 HintB) fired at age 139 s while every presented frame stayed
# exact black until ~203 s, so input keyed on HintB would have landed on a
# black screen. Same two rules as the black gate: POST-STEP-ENTRY frames only
# (the probe builder owns that filter), and a CONTINUOUS run - one exact-black
# frame resets it. "Visible" means exactly "not full-frame exact black"
# (Test-FrameExactBlack = 0); it says nothing about WHAT is shown.
# Same $P contract as Invoke-SustainedBlackGate.
# Returns @{ ok; reason; framesSeen; everVisible; spanMs }.
function Invoke-SustainedVisibleGate([int]$RequireMs, [int]$TimeoutMs, $P) {
    $deadline    = (& $P.NowMs) + [int64]$TimeoutMs
    $runStart    = $null
    $lastId      = $null
    $everVisible = $false
    $framesSeen  = 0
    $span        = 0
    while ((& $P.NowMs) -lt $deadline) {
        $f = & $P.Newest
        if ($f -and $f.id -ne $lastId) {
            $lastId = $f.id
            $framesSeen++
            if ([int]$f.black -lt 0) {
                # unreadable (half-written): neither extends nor breaks a run
            } elseif ([int]$f.black -eq 0) {
                $everVisible = $true
                if ($null -eq $runStart) {
                    $runStart = [int64]$f.timeMs
                    & $P.Note ("visible-gate: run STARTED at {0} (not exact black)" -f $f.name)
                }
                $span = [int64]$f.timeMs - $runStart
                if ($span -ge $RequireMs) {
                    & $P.Note ("visible-gate: SUSTAINED {0} ms of post-entry non-black frames over {1} frames" -f $span, $framesSeen)
                    return @{ ok = $true; reason = 'sustained'; framesSeen = $framesSeen
                              everVisible = $true; spanMs = $span }
                }
            } else {
                if ($null -ne $runStart) {
                    & $P.Note ("visible-gate: run BROKEN by {0} (full-frame exact black) - restarting" -f $f.name)
                }
                $runStart = $null
                $span = 0
            }
        }
        & $P.Sleep 500
    }
    if (-not $everVisible) {
        $reason = 'never-visible'
        & $P.Note ("visible-gate: FAILED - no post-entry frame was non-black within {0} ms ({1} frames)." -f $TimeoutMs, $framesSeen)
    } else {
        $reason = 'not-sustained'
        & $P.Note ("visible-gate: FAILED - non-black seen but never sustained for {0} ms within {1} ms ({2} frames)." -f $RequireMs, $TimeoutMs, $framesSeen)
    }
    return @{ ok = $false; reason = $reason; framesSeen = $framesSeen
              everVisible = $everVisible; spanMs = $span }
}

# ── iter-456q: THE VERIFIED KEY RELEASE, as an injectable body ──────────────
# The director's second blocker: the production key-up paths discarded
# Send-Key's result and removed keys from the held list BEFORE the release was
# established, and the tests did not exercise those paths at all. The logic
# lives here so the self-test drives it with fake probes, exactly as
# Invoke-HardenedDialog is driven.
#
# $P = @{ IsDown = { param($vk) } -> bool
#         SendUp = { param($vk,$ext) } -> bool (SendInput succeeded)
#         Sleep  = { param($ms) }
#         Note   = { param($m) } }
# Returns @{ released; attempts; sendFailures }.
function Invoke-VerifiedRelease([int]$Vk, [bool]$Ext, [int]$Attempts, $P) {
    $sendFailures = 0
    for ($i = 1; $i -le $Attempts; $i++) {
        if (-not (& $P.IsDown $Vk)) {
            return @{ released = $true; attempts = $i - 1; sendFailures = $sendFailures }
        }
        & $P.Note ("key-up NOT established for vk=0x{0:X2} (attempt {1}/{2}) - resending" -f $Vk, $i, $Attempts)
        if (-not (& $P.SendUp $Vk $Ext)) { $sendFailures++ }
        & $P.Sleep 40
    }
    if (& $P.IsDown $Vk) {
        & $P.Note ("STUCK KEY: vk=0x{0:X2} still reads DOWN after {1} release attempts" -f $Vk, $Attempts)
        return @{ released = $false; attempts = $Attempts; sendFailures = $sendFailures }
    }
    return @{ released = $true; attempts = $Attempts; sendFailures = $sendFailures }
}

# ── iter-456q: RELEASE-ALL, as an injectable body ───────────────────────────
# The director's blocker: Release-AllKeys called Clear() unconditionally, so a
# key that could NOT be proven released was forgotten anyway - the bookkeeping
# was discarded at exactly the moment it mattered. Keys that fail to release now
# STAY on the returned list, and the caller keeps them.
#
# $P = @{ SendUp = { param($vk,$ext) } -> bool
#         IsDown = { param($vk) } -> bool
#         Sleep  = { param($ms) }
#         Note   = { param($m) } }
# Returns @{ remaining = @(unreleased); stuck = N; sendFailures = N }.
# ⚠ THIS BODY DOES NOT TALLY SEND FAILURES. The SendUp probe owns them, so
# there is exactly ONE source of truth: production's Send-Key increments the
# health record at the point of failure, and a second tally here would double
# count every release failure - the defect that shipped once already.
function Invoke-ReleaseAllBody($Held, [int]$Attempts, $P) {
    $remaining = @()
    $stuck = 0
    foreach ($k in @($Held)) {
        [void](& $P.SendUp $k.vk $k.ext)
        $r = Invoke-VerifiedRelease $k.vk $k.ext $Attempts $P
        if (-not $r.released) {
            $stuck++
            $remaining += $k
            & $P.Note ("Release-AllKeys: vk=0x{0:X2} NOT released - RETAINED on the held list" -f $k.vk)
        }
    }
    return @{ remaining = $remaining; stuck = $stuck }
}

# ── iter-456q: CLEANUP + FINALIZATION, composed ─────────────────────────────
# The director's last blocker: the send-failure counter was captured BEFORE
# cleanup and the cleanup adapter returned a hardcoded zero, so a cleanup
# SendInput=0 produced finalOk=true / finalSendFailures=0 / globalAfterRelease=1
# / "ALL STEPS PASSED". The previous test missed it because it injected the
# desired count into the finalizer instead of composing the real adapter.
#
# The fix is structural, not arithmetic: `$Health` is the ONE canonical record,
# the cleanup mutates it through the same probes production uses, and this body
# reads it ONCE, AFTER cleanup. There is no snapshot to go stale and no second
# tally to double count.
#
# $Health = @{ sendFailures = N; stuckKeys = N }   (mutated in place)
# $P      = @{ SendUp; IsDown; Sleep; Note }       (SendUp increments Health)
# Returns @{ ok; failReason; verdictLine; sendFailures; stuckKeys; remaining }.
function Invoke-CleanupAndFinalize([bool]$StepOk, [string]$FailReason,
                                   $Held, [int]$Attempts, $Health, $P) {
    $rel = Invoke-ReleaseAllBody $Held $Attempts $P
    # ⚠ DERIVED AND ASSIGNED, never accumulated. `+=` counted one physical key
    # TWICE - once when a per-step release failed and again when cleanup found
    # it still held. stuckKeys is defined as THE NUMBER OF KEYS STILL UNRELEASED
    # AT FINALIZATION, which is unambiguous and cannot double.
    #
    # A key that failed a per-step release and was then rescued during cleanup
    # is NOT stuck: it ends up released. Any SendInput failures its retries
    # incurred are already counted in sendFailures, so the hiccup is not lost.
    $Health.stuckKeys = @($rel.remaining).Count

    # READ ONCE, AFTER CLEANUP. Anything captured earlier could not see a
    # failure that happened during the unwind.
    $sendFailures = [int]$Health.sendFailures
    $stuck        = [int]$Health.stuckKeys
    & $P.Note ("input health: sendInputFailures={0} stuckKeys={1}" -f $sendFailures, $stuck)

    $ok = $StepOk
    $reason = $FailReason
    if ($ok -and ($sendFailures -gt 0 -or $stuck -gt 0)) {
        $ok = $false
        $reason = "input delivery was not healthy (sendInputFailures=$sendFailures stuckKeys=$stuck) - the run cannot be trusted as a measurement"
    }
    $line = if ($ok) { "ALL STEPS PASSED" } else { "FAILED: $reason" }
    & $P.Note $line
    return @{ ok = $ok; failReason = $reason; verdictLine = $line
              sendFailures = $sendFailures; stuckKeys = $stuck
              remaining = $rel.remaining }
}

# ── iter-456q: the black-gate PROBE BUILDER ─────────────────────────────────
# Extracted so a test can prove the frameDir and notBefore BINDINGS, not just
# the gate body. The director asked for exactly that: the wrapper is where a
# wrong directory or a forgotten post-entry filter would hide.
#
# $Sampler = { param($path) } -> 1 / 0 / -1 (injected so tests need no BMPs;
# production passes Test-FrameExactBlack).
function New-BlackGateProbes([string]$FrameDir, [datetime]$NotBefore,
                             $Sampler, $Now, $Sleep, $Note) {
    @{
        NowMs  = $Now
        Sleep  = $Sleep
        Note   = $Note
        Newest = {
            $f = Get-ChildItem -Path $FrameDir -Filter *.bmp -ErrorAction SilentlyContinue |
                 Where-Object { $_.LastWriteTime -ge $NotBefore } |
                 Sort-Object LastWriteTime | Select-Object -Last 1
            if (-not $f) { return $null }
            @{ id = $f.FullName; name = $f.Name
               timeMs = [int64]($f.LastWriteTime.ToUniversalTime() - [DateTime]'1970-01-01').TotalMilliseconds
               black = (& $Sampler $f.FullName) }
        }.GetNewClosure()
    }
}

# ── iter-456q: the SEND-KEY RESULT adapter, minus Win32 ─────────────────────
# The director's point: a test that hand-increments the health record could not
# tell a broken Send-Key from a working one. This is Send-Key's whole
# non-Win32 half - the part that decides what a SendInput return MEANS and
# records it - so a test can drive the real adapter with n=0.
#
# Returns $true when the event was inserted; on failure increments $Health and
# names the Win32 error.
function Invoke-SendKeyResult([int]$Inserted, [int]$Win32Error, [int]$Vk,
                              [bool]$Up, $Health, $Note) {
    if ($Inserted -eq 1) { return $true }
    $Health.sendFailures++
    & $Note ("SEND-KEY FAILED: SendInput inserted {0} of 1 for vk=0x{1:X2} up={2} - Win32 error {3}" -f $Inserted, $Vk, $Up, $Win32Error)
    return $false
}

# ── iter-456q: OUTCOME / EXIT MAPPING ───────────────────────────────────────
# Named by the director's bar item 2 and previously untested: the finalizer's
# boolean must reach BOTH the persisted outcome record and the process exit
# code, with no path where a failed run exits 0.
function Resolve-RunOutcome($Fin) {
    $ok = [bool]$Fin.ok
    return @{ ok = $ok
              exitCode = $(if ($ok) { 0 } else { 1 })
              result = $(if ($ok) { 'SUCCESS' } else { 'FAILURE' })
              failReason = [string]$Fin.failReason }
}

# -- iter-458: LIVENESS DURING AND AFTER THE DWELL -----------------------------
# iter458_hr19b2: the game died 9 s into a 240 s observe-gameplay dwell. The old
# dwell loop broke on a lost window, printed "DONE", and the run finalized as
# SUCCESS - a dead process graded as a completed measurement. A dwell is now a
# measurement window the game must SURVIVE: death or window loss at any tick,
# or at the end, fails the step and names the elapsed time and exit code.
#
# The production liveness probe. Exit-first, through the same Get-SrAliveReason
# the hardened body uses. $IsWindow = { param($h) } -> bool (injected so tests
# need no HWND). The function is captured as a scriptblock because a
# GetNewClosure block cannot see script-scope functions.
function New-LivenessProbe($Proc, $Hwnd, $IsWindow) {
    $aliveFn = ${function:Get-SrAliveReason}
    {
        $he = [bool]$Proc.HasExited
        & $aliveFn $he $(if ($he) { $Proc.ExitCode } else { 0 }) ([bool](& $IsWindow $Hwnd)) $Hwnd
    }.GetNewClosure()
}

# $P = @{ NowMs; Alive; Pump; Sleep; Note }. Alive returns $null or a reason.
# Returns @{ ok; reason; elapsedMs; ticks }.
function Invoke-DwellBody([string]$Name, [int]$DwellMs, [int]$TickMs, $P) {
    $t0 = [int64](& $P.NowMs)
    $ticks = 0
    while ($true) {
        $el = [int64](& $P.NowMs) - $t0
        & $P.Pump
        $why = & $P.Alive
        if ($why) {
            $msg = "step '$Name': the game did not survive the dwell - $why after $el of $DwellMs ms"
            & $P.Note $msg
            return @{ ok = $false; reason = $msg; elapsedMs = $el; ticks = $ticks }
        }
        if ($el -ge $DwellMs) { return @{ ok = $true; reason = $null; elapsedMs = $el; ticks = $ticks } }
        & $P.Sleep $TickMs
        $ticks++
    }
}

# 2026-10-18 (step 4, set s4h_wg_rel1): a wait for a log line must also watch
# the game. Wait-For used to poll only the log: the release exe crashed 533 s
# into the chapters step's 7,740 s precondition wait and the driver sat out the
# rest of it, holding the ROG's game slot. $WaitProbes = @{ NowMs; Cond; Alive;
# Pump; Sleep }: Alive returns $null or a reason (the watched process exited;
# $null when no process is watched, e.g. between a MODS-menu restart's parent
# and child). After an exit the condition is tested once more on a fresh pump:
# the lines the game wrote before it died still count. The timeout path keeps
# the old contract (poll until the deadline, then one last pump + test).
# Returns @{ ok; exited; reason; elapsedMs }. (Locals are wb-prefixed: the
# condition scriptblocks resolve the caller's variables through this scope.)
function Invoke-WaitBody([int]$TimeoutMs, [int]$PollMs, $WaitProbes) {
    $wbT0 = [int64](& $WaitProbes.NowMs)
    while (([int64](& $WaitProbes.NowMs) - $wbT0) -lt $TimeoutMs) {
        & $WaitProbes.Pump
        if (& $WaitProbes.Cond) {
            return @{ ok = $true; exited = $false; reason = $null; elapsedMs = [int64](& $WaitProbes.NowMs) - $wbT0 }
        }
        $wbWhy = & $WaitProbes.Alive
        if ($wbWhy) {
            & $WaitProbes.Pump
            $wbOk = [bool](& $WaitProbes.Cond)
            return @{ ok = $wbOk; exited = (-not $wbOk); reason = $(if ($wbOk) { $null } else { $wbWhy })
                      elapsedMs = [int64](& $WaitProbes.NowMs) - $wbT0 }
        }
        & $WaitProbes.Sleep $PollMs
    }
    & $WaitProbes.Pump
    $wbOk = [bool](& $WaitProbes.Cond)
    return @{ ok = $wbOk; exited = $false; reason = $(if ($wbOk) { $null } else { 'timeout' })
              elapsedMs = [int64](& $WaitProbes.NowMs) - $wbT0 }
}

# Immediately before finalization: a run whose steps all passed is still not a
# success if the game is gone by the time the verdict is taken.
function Invoke-PreFinalLiveness([bool]$StepOk, [string]$FailReason, $Alive) {
    if (-not $StepOk) { return @{ ok = $false; failReason = $FailReason } }
    $why = & $Alive
    if ($why) {
        return @{ ok = $false; failReason = "the game was not alive at finalization - $why" }
    }
    return @{ ok = $true; failReason = $FailReason }
}

# ---------------------------------------------------------------------------
# iter-463 (devtools phase 1b item 6): the GENERAL scenario contract. Until now
# only 'hardened' scenarios were validated; every other scenario went straight
# to a live roll, so a typo'd field (silently $null), a key the driver does not
# know (found only at that step, minutes into a roll), a regex that does not
# compile, or an expectExitCode step that is not last cost a whole game slot.
# Assert-SrScenario runs for EVERY scenario, before -ValidateOnly returns and
# before any directory, env or process action. Field names are canonical
# (case-sensitive): the driver reads them case-insensitively, so a different
# casing would still work, but it is refused here so the shipped files stay
# greppable. Returns @{ ok; reason }.
# ---------------------------------------------------------------------------
# The pad keys sr_drive.ps1 can send. sr_drive.ps1 checks its $KEYS table
# against this list at start-up (a mismatch exits 2), so the two cannot drift.
$script:SR_KEY_NAMES = @('ENTER','RETURN','ESCAPE','BACK','UP','DOWN','LEFT','RIGHT',
                         'LB','RB','A','B','X','Y','F9',
                         'F6','F7','F8','KEY_W','KEY_A','KEY_S','KEY_D','KEY_I','KEY_J','KEY_K','KEY_L','KEY_R','KEY_F',
                         'SPACE','TAB','PAGEUP','PAGEDOWN','F12')
$script:SR_TOP_FIELDS  = @('name','purpose','gates','steps','requires','coverMovies')
$script:SR_STEP_FIELDS = @('name','purpose','failureArtifact',
                           'await','timeoutMs',
                           'key','holdMs','maxPulses','gapMs','holdUntilConfirmed','confirm','confirmMs',
                           'expect','expectMs','expectFresh','optional','singleDeadline',
                           'requireBlackMs','blackTimeoutMs','requireVisibleMs','visibleTimeoutMs',
                           'dwellMs','expectExitCode','exitTimeoutMs','mouse','followRestart','cutsceneSkip',
                           'onlyIfObserved','expectSince')
# 2026-10-14 chapter seeds: `onlyIfObserved` = a regex; the step runs only when the line the latest `expect` observed
# matches it, else it is skipped (logged). perf_run --from-save: A = METROPOLIS only after a load that landed on the
# METROPOLIS / FORTRESS carousel, never in play after a load that landed straight in the HUD.
# 2026-10-15 (Astra round 1 on tools/chapter-saves, P1): `expectSince` = the name of an EARLIER step; the step's expect
# counts only lines that arrived after that step was entered (Get-SrArmPlan below).
$script:SR_REGEX_FIELDS = @('await','expect','confirm','onlyIfObserved')
$script:SR_MS_FIELDS    = @('timeoutMs','holdMs','maxPulses','gapMs','confirmMs','expectMs',
                            'requireBlackMs','blackTimeoutMs','requireVisibleMs','visibleTimeoutMs',
                            'dwellMs','exitTimeoutMs')
$script:SR_BOOL_FIELDS  = @('holdUntilConfirmed','expectFresh','optional','singleDeadline','followRestart','cutsceneSkip')
$script:SR_TEXT_FIELDS  = @('name','purpose','failureArtifact','key','mouse','expectSince')
# field -> the field it only makes sense with (the driver ignores it otherwise)
$script:SR_NEEDS = @{ holdMs = 'key'; maxPulses = 'key'; gapMs = 'key'; holdUntilConfirmed = 'key'
                      confirm = 'key'; confirmMs = 'key'; expectFresh = 'expect'; expectMs = 'expect'
                      blackTimeoutMs = 'requireBlackMs'; visibleTimeoutMs = 'requireVisibleMs'
                      exitTimeoutMs = 'expectExitCode'; cutsceneSkip = 'key'; expectSince = 'expect' }
$script:SR_REQUIRES_FIELDS = @('contentSeed')

function Test-SrIsInt($v) { return ($v -is [int]) -or ($v -is [long]) -or ($v -is [int16]) }

# iter-464: the `mouse` step grammar (sr_drive.ps1 Invoke-MouseScript). Commands separated
# by ';', points in Apt stage pixels (0..1280, 0..720):
#   move X,Y | click [X,Y] | rclick [X,Y] | mclick [X,Y] | wheel N | drag X1,Y1 X2,Y2 | wait MS
#   | moveitem I [left|center|right] | clickitem I [...]   (the game's item I in the last snapshot)
#   | movehint BTN | clickhint BTN                         (lane E E5: the button hint BTN, below)
# Lane E E5 (step 5): the button hints of the last [MOUSE_UI] snapshot, by the AS key code
# mouse_ui logs for each (`[MOUSE_UI]   hint i key=K rect=(x0,y0)-(x1,y1)`; hintIconKey).
$script:SR_HINT_KEYS = @{ a = 302; b = 304; x = 305; y = 303; start = 301; select = 300 }
# One hint line -> @{ key; x0; y0; x1; y1 }, or $null.
function ConvertFrom-SrMouseHintLine([string]$line) {
    if ($line -notmatch '\[MOUSE_UI\]\s+hint \d+ key=(\d+) rect=\((-?[0-9.]+),(-?[0-9.]+)\)-\((-?[0-9.]+),(-?[0-9.]+)\)') { return $null }
    $ci = [cultureinfo]::InvariantCulture
    return @{ key = [int]$Matches[1]
              x0 = [double]::Parse($Matches[2], $ci); y0 = [double]::Parse($Matches[3], $ci)
              x1 = [double]::Parse($Matches[4], $ci); y1 = [double]::Parse($Matches[5], $ci) }
}
# The stage centre of button <name>'s hint in $hints (key code -> hint), or $null.
function Get-SrHintPoint($hints, [string]$name) {
    $n = $name.ToLower()
    if (-not $script:SR_HINT_KEYS.ContainsKey($n)) { return $null }
    $code = [int]$script:SR_HINT_KEYS[$n]
    if (-not $hints.ContainsKey($code)) { return $null }
    $h = $hints[$code]
    $x = 0.5 * ($h.x0 + $h.x1)
    $y = 0.5 * ($h.y0 + $h.y1)
    return ,@([Math]::Max(0.0, [Math]::Min(1280.0, $x)), [Math]::Max(0.0, [Math]::Min(720.0, $y)))
}
# The hint half of sr_drive's [MOUSE_UI] tracker (sr_drive.ps1 Update-MouseItems): a snapshot line starts an EMPTY
# set (a snapshot whose hints are unknown logs no hint line, so the previous page's hints never survive it); each hint
# line adds its key, the first per key winning (mouse_ui logs the bar in order). Returns the set to keep.
function Update-SrHintSet($hints, [string]$line) {
    if ($line -match '\[MOUSE_UI\] snapshot #\d+ ') { return @{} }
    $hl = ConvertFrom-SrMouseHintLine $line
    if ($null -ne $hl -and -not $hints.ContainsKey($hl.key)) { $hints[$hl.key] = $hl }
    return $hints
}
function Test-SrMouseScript([string]$m) {
    if (-not $m.Trim()) { return @{ ok = $false; reason = "empty mouse script" } }
    foreach ($raw in $m.Split(';')) {
        $c = $raw.Trim()
        if (-not $c) { continue }
        $parts = @($c -split ' +')
        $verb = $parts[0].ToLower()
        $args2 = @($parts | Select-Object -Skip 1)
        $pts = @($args2 | Where-Object { $_ -match '^-?[0-9.]+,-?[0-9.]+$' })
        foreach ($p in $pts) {
            $xy = $p.Split(',')
            if ([double]$xy[0] -lt 0 -or [double]$xy[0] -gt 1280 -or [double]$xy[1] -lt 0 -or [double]$xy[1] -gt 720) {
                return @{ ok = $false; reason = "'$c': point $p is outside the 1280x720 stage" }
            }
        }
        if ($verb -eq 'move') {
            if ($args2.Count -ne 1 -or $pts.Count -ne 1) { return @{ ok = $false; reason = "'$c': move takes one X,Y" } }
        } elseif (@('click','rclick','mclick') -contains $verb) {
            if ($args2.Count -gt 1 -or $args2.Count -ne $pts.Count) { return @{ ok = $false; reason = "'$c': $verb takes at most one X,Y" } }
        } elseif ($verb -eq 'wheel') {
            if ($args2.Count -ne 1 -or $args2[0] -notmatch '^-?[1-9][0-9]?$') { return @{ ok = $false; reason = "'$c': wheel takes a non-zero notch count" } }
        } elseif ($verb -eq 'drag') {
            if ($args2.Count -ne 2 -or $pts.Count -ne 2) { return @{ ok = $false; reason = "'$c': drag takes two X,Y" } }
        } elseif ($verb -eq 'wait') {
            if ($args2.Count -ne 1 -or $args2[0] -notmatch '^[1-9][0-9]{0,5}$') { return @{ ok = $false; reason = "'$c': wait takes milliseconds" } }
        } elseif (@('moveitem','clickitem') -contains $verb) {
            # iter-464: aim at the game's item <index> from the latest [MOUSE_UI] snapshot
            if ($args2.Count -lt 1 -or $args2.Count -gt 2 -or $args2[0] -notmatch '^[0-9]{1,2}$' -or
                ($args2.Count -eq 2 -and @('left','center','right') -notcontains $args2[1].ToLower())) {
                return @{ ok = $false; reason = "'$c': $verb takes an item index and optionally left|center|right" }
            }
        } elseif (@('movehint','clickhint') -contains $verb) {
            if ($args2.Count -ne 1 -or -not $script:SR_HINT_KEYS.ContainsKey($args2[0].ToLower())) {
                return @{ ok = $false; reason = "'$c': $verb takes a button: A, B, X, Y, START or SELECT" }
            }
        } else {
            return @{ ok = $false; reason = "unknown mouse verb '$verb'" }
        }
    }
    return @{ ok = $true; reason = 'ok' }
}

function Assert-SrScenario($scenario) {
    function scnFail([string]$why) { return @{ ok = $false; reason = $why } }
    if ($null -eq $scenario -or $scenario -isnot [System.Management.Automation.PSCustomObject]) {
        return scnFail "the scenario is not a JSON object"
    }
    foreach ($p in $scenario.PSObject.Properties) {
        if ($script:SR_TOP_FIELDS -cnotcontains $p.Name) { return scnFail "unknown top-level field '$($p.Name)'" }
    }
    if (-not ($scenario.name -is [string]) -or -not $scenario.name.Trim()) { return scnFail "'name' must be a non-empty string" }
    if ($null -ne $scenario.purpose -and -not ($scenario.purpose -is [string])) { return scnFail "'purpose' must be a string" }
    if ($null -ne $scenario.coverMovies -and -not ($scenario.coverMovies -is [bool])) { return scnFail "'coverMovies' must be true or false" }
    if ($null -ne $scenario.gates) {
        if (-not ($scenario.gates -is [string])) { return scnFail "'gates' must be a string of NAME=VALUE pairs" }
        foreach ($pair in $scenario.gates.Split(',')) {
            if (-not $pair.Trim()) { continue }
            if ($pair -cnotmatch '^\s*[A-Z][A-Z0-9_]*=[^,]*$') { return scnFail "gate '$pair' is not NAME=VALUE (upper-case name)" }
        }
    }
    if ($null -ne $scenario.requires) {
        if ($scenario.requires -isnot [System.Management.Automation.PSCustomObject]) { return scnFail "'requires' must be an object" }
        foreach ($p in $scenario.requires.PSObject.Properties) {
            if ($script:SR_REQUIRES_FIELDS -cnotcontains $p.Name) { return scnFail "unknown requirement '$($p.Name)'" }
            if (-not ($p.Value -is [bool])) { return scnFail "requirement '$($p.Name)' must be true or false" }
        }
    }
    $steps = @($scenario.steps)
    if ($null -eq $scenario.steps -or $steps.Count -eq 0) { return scnFail "'steps' must be a non-empty array" }
    $seen = @{}
    for ($i = 0; $i -lt $steps.Count; $i++) {
        $s = $steps[$i]
        $at = "step $($i + 1)"
        if ($s -isnot [System.Management.Automation.PSCustomObject]) { return scnFail "$at is not an object" }
        foreach ($p in $s.PSObject.Properties) {
            if ($script:SR_STEP_FIELDS -cnotcontains $p.Name) { return scnFail "$at has unknown field '$($p.Name)'" }
        }
        if (-not ($s.name -is [string]) -or -not $s.name.Trim()) { return scnFail "$at has no name" }
        $at = "step '$($s.name)'"
        if ($seen.ContainsKey($s.name)) { return scnFail "${at}: duplicate step name" }
        $seen[$s.name] = $true
        foreach ($f in $script:SR_TEXT_FIELDS) {
            if ($null -ne $s.$f -and -not ($s.$f -is [string])) { return scnFail "${at}: '$f' must be a string" }
        }
        foreach ($f in $script:SR_REGEX_FIELDS) {
            if ($null -eq $s.$f) { continue }
            if (-not ($s.$f -is [string]) -or -not $s.$f) { return scnFail "${at}: '$f' must be a non-empty regex string" }
            try { [void][regex]::new([string]$s.$f) } catch { return scnFail "${at}: '$f' does not compile: $($_.Exception.InnerException.Message)" }
        }
        foreach ($f in $script:SR_MS_FIELDS) {
            if ($null -eq $s.$f) { continue }
            if (-not (Test-SrIsInt $s.$f) -or [int64]$s.$f -le 0) { return scnFail "${at}: '$f' must be a positive integer (got '$($s.$f)')" }
        }
        foreach ($f in $script:SR_BOOL_FIELDS) {
            if ($null -ne $s.$f -and -not ($s.$f -is [bool])) { return scnFail "${at}: '$f' must be true or false" }
        }
        if ($null -ne $s.expectExitCode -and -not (Test-SrIsInt $s.expectExitCode)) { return scnFail "${at}: 'expectExitCode' must be an integer" }
        foreach ($f in $script:SR_NEEDS.Keys) {
            $need = $script:SR_NEEDS[$f]
            if ($null -ne $s.$f -and $null -eq $s.$need) { return scnFail "${at}: '$f' is set without '$need' (the driver would ignore it)" }
        }
        if ($null -ne $s.key -and $script:SR_KEY_NAMES -notcontains ([string]$s.key).ToUpper()) {
            return scnFail "${at}: unknown key '$($s.key)' (known: $($script:SR_KEY_NAMES -join ' '))"
        }
        if ($null -ne $s.mouse) {
            $mv = Test-SrMouseScript ([string]$s.mouse)
            if (-not $mv.ok) { return scnFail "${at}: mouse: $($mv.reason)" }
            if ($null -ne $s.key) { return scnFail "${at}: a step takes a key or a mouse script, not both" }
        }
        $acts = @('await','expect','key','mouse','dwellMs','requireBlackMs','requireVisibleMs','expectExitCode','followRestart') |
                Where-Object { $null -ne $s.$_ }
        if (@($acts).Count -eq 0) { return scnFail "$at does nothing (no await, expect, key, dwell, gate or exit check)" }
        if ($null -ne $s.expectExitCode) {
            if ($i -ne $steps.Count - 1) { return scnFail "${at}: an expectExitCode step must be the last step (the game is gone)" }
            if ($null -ne $s.key -or $null -ne $s.await -or $null -ne $s.expect) { return scnFail "${at}: an expectExitCode step takes no key, await or expect" }
        }
        if ($null -ne $s.expectSince) {
            # the boundary step must run before this one, every time: an earlier step that is never skipped
            $since = [string]$s.expectSince
            if (-not $since.Trim()) { return scnFail "${at}: 'expectSince' must name an earlier step" }
            if ($since -ceq $s.name) { return scnFail "${at}: 'expectSince' names the step itself (use expectFresh)" }
            $ref = $null
            for ($k = 0; $k -lt $i; $k++) { if ([string]$steps[$k].name -ceq $since) { $ref = $steps[$k] } }
            if ($null -eq $ref) { return scnFail "${at}: 'expectSince' names no earlier step ('$since')" }
            if ($s.expectFresh) { return scnFail "${at}: 'expectSince' and 'expectFresh' are two different boundaries - pick one" }
            if ($ref.cutsceneSkip -or $ref.onlyIfObserved) {
                return scnFail "${at}: 'expectSince' names step '$since', which can be skipped (cutsceneSkip / onlyIfObserved): its boundary might never be armed"
            }
        }
    }
    return @{ ok = $true; reason = 'ok' }
}

# ---------------------------------------------------------------------------
# 2026-10-15 (Astra round 1 on tools/chapter-saves, P1): a step BOUNDARY. A step with `expectSince: "<earlier step>"`
# counts its expect only in lines that arrived after that earlier step was ENTERED, before its action:
#   - no boundary counts every line of the attempt (a HUD line from before the load passed 'load-lands');
#   - expectFresh counts only lines after THIS step began, which is too late when the line follows the previous
#     step's observation by milliseconds (the HUD landing is logged 11 ms after the load event: roll
#     archive/saves/_verify/01_ch01_after_mstobj03_*_10142258 - one pump read both lines, so a fresh base would hide it).
# perf_run --from-save arms the boundary at 'load-slot' (before LOAD GAME's slot press): the landing and the gameplay
# context must come after the load action. Opt-in: a scenario without expectSince arms nothing and pumps nothing extra.
# Get-SrArmPlan: step name -> the expect patterns of the later steps that name it.
function Get-SrArmPlan($Steps) {
    $plan = @{}
    foreach ($s in @($Steps)) {
        if ($s.expectSince -and $s.expect) {
            $k = [string]$s.expectSince
            if (-not $plan.ContainsKey($k)) { $plan[$k] = @() }
            $plan[$k] += [string]$s.expect
        }
    }
    return $plan
}

# The boundary snapshot taken at the armed step's entry (after a log pump): each pattern's hit count.
# $CountOf = { param($p) } -> int.
function New-SrArmSnapshot($Patterns, $CountOf) {
    $snap = @{}
    foreach ($p in @($Patterns)) { $snap[$p] = [int](& $CountOf $p) }
    return $snap
}

# The expect base of a step with expectSince -> @{ ok; base; reason }. A boundary step that never ran armed nothing:
# the step fails instead of falling back to the whole attempt.
function Get-SrSinceBase($Step, $Armed) {
    $since = [string]$Step.expectSince
    if (-not $Armed.ContainsKey($since)) {
        return @{ ok = $false; base = 0
                  reason = "expectSince '$since': that step never ran, so its boundary was never armed - THIS IS THE BLOCKER" }
    }
    return @{ ok = $true; base = [int]$Armed[$since][[string]$Step.expect]; reason = $null }
}

# 2026-10-14 `onlyIfObserved` (the step loop's skip test): $true = the step runs.
function Test-SrOnlyIfObserved($Step, [string]$LastObserved) {
    if (-not $Step.onlyIfObserved) { return $true }
    return [bool]($LastObserved -match [string]$Step.onlyIfObserved)
}

# The save seed a run WILL use, with the driver's own precedence: the scenario
# gates are applied first, then -Gates (a later NAME=VALUE wins), and only when
# neither sets SR_CONTENT_DIR does the caller's environment count. Pure: reads
# nothing but its arguments.
function Get-SrSeedFromGates([string]$ScenarioGates, [string]$CliGates, [string]$EnvSeed) {
    $seed = $EnvSeed
    foreach ($chunk in @($ScenarioGates, $CliGates)) {
        if (-not $chunk) { continue }
        foreach ($pair in $chunk.Split(',')) {
            $kv = $pair.Split('=', 2)
            # -eq, not -ceq: the driver's apply loop sets Env: names, which Windows
            # treats case-insensitively, so 'sr_content_dir=' IS the seed at run time
            if ($kv.Count -eq 2 -and $kv[0].Trim() -eq 'SR_CONTENT_DIR') { $seed = $kv[1].Trim() }
        }
    }
    return $seed
}

# ---------------------------------------------------------------------------
# iter-467 forcing (owner 2026-10-11, via the orchestrator): rolls SKIP CUTSCENES - pressing the game's skip input
# is a natural player action, not forcing. A step with "cutsceneSkip": true is a skip step (e.g. A pulses during the
# new game's opening FMV until the guest's skip edge `[EV187] ... ACCEPTED-KILL`); it runs only while skipping is ON.
# ON by default (gate / proof / A-B / planner rolls). OFF for rolls that must cover movies: the scenario says
# "coverMovies": true, or its name / file names a full or whole-run roll (whole-run meter coverage counts movies;
# movie-stall measurements), or the caller says so: -SkipCutscenes off, or SR_DRIVE_SKIP_CUTSCENES=0 (environment,
# scenario gates or -Gates; the last one named wins). Returns @{ on; why }.
# ---------------------------------------------------------------------------
function Resolve-SrCutsceneSkip([string]$Mode, $Scenario, [string]$ScenarioPath, [string]$ScenarioGates, [string]$CliGates, [string]$EnvValue) {
    $m = ([string]$Mode).Trim().ToLower()
    if ($m -eq 'on' -or $m -eq '1') { return @{ on = $true; why = '-SkipCutscenes on' } }
    if ($m -eq 'off' -or $m -eq '0') { return @{ on = $false; why = '-SkipCutscenes off' } }
    $v = $EnvValue; $src = 'the environment'
    foreach ($pair in @(([string]$ScenarioGates).Split(',')) + @(([string]$CliGates).Split(','))) {
        $kv = $pair.Split('=', 2)
        if ($kv.Count -eq 2 -and $kv[0].Trim() -eq 'SR_DRIVE_SKIP_CUTSCENES') { $v = $kv[1].Trim(); $src = 'the gates' }
    }
    if ($v -eq '0') { return @{ on = $false; why = "SR_DRIVE_SKIP_CUTSCENES=0 ($src)" } }
    if ($v -eq '1') { return @{ on = $true; why = "SR_DRIVE_SKIP_CUTSCENES=1 ($src)" } }
    if ($null -ne $Scenario -and $Scenario.coverMovies -eq $true) { return @{ on = $false; why = 'the scenario covers movies (coverMovies)' } }
    $names = ("{0} {1}" -f $(if ($null -ne $Scenario) { $Scenario.name } else { '' }), [System.IO.Path]::GetFileName([string]$ScenarioPath))
    if ($names -match '(?i)full|whole_?run') { return @{ on = $false; why = "a full / whole-run scenario ($($names.Trim())) covers movies" } }
    return @{ on = $true; why = 'default (gate / proof / A-B / planner rolls skip cutscenes)' }
}

# A scenario's `requires` against the invocation. contentSeed=true: the run is a
# LOAD from a save, so a missing seed (or a seed that holds no profile folder)
# is refused BEFORE any directory, env or process action - without a save the
# retail LOAD A GAME slot picker shows three disabled EMPTY SLOTs and the roll
# would burn its whole budget proving nothing. Returns @{ ok; reason }.
function Test-SrScenarioRequirements($scenario, [string]$Seed) {
    if ($null -eq $scenario.requires -or -not [bool]$scenario.requires.contentSeed) { return @{ ok = $true; reason = 'no requirements' } }
    if (-not $Seed) { return @{ ok = $false; reason = "the scenario requires a save seed (requires.contentSeed): pass -Gates SR_CONTENT_DIR=<content root> (docs/SAVES.md; python tools/import_xenia_saves.py library path <label>)" } }
    if (-not (Test-Path -LiteralPath $Seed -PathType Container)) { return @{ ok = $false; reason = "the save seed '$Seed' is not a directory" } }
    $profiles = @(Get-ChildItem -LiteralPath $Seed -Directory -ErrorAction SilentlyContinue |
                  Where-Object { $_.Name -match '^[0-9A-Fa-f]{16}$' })
    if ($profiles.Count -eq 0) { return @{ ok = $false; reason = "the save seed '$Seed' holds no 16-hex-digit profile folder (the port's content-root layout, docs/SAVES.md)" } }
    # the game's saved-game packages: <xuid>/454107ED/00000001/<package>/ (docs/SAVES.md)
    $pkgs = @($profiles | ForEach-Object {
        $d = Join-Path $_.FullName '454107ED\00000001'
        if (Test-Path -LiteralPath $d -PathType Container) { Get-ChildItem -LiteralPath $d -Directory -ErrorAction SilentlyContinue }
    })
    if ($pkgs.Count -eq 0) { return @{ ok = $false; reason = "the save seed '$Seed' holds no saved-game package (<xuid>\454107ED\00000001\<package>\, docs/SAVES.md)" } }
    return @{ ok = $true; reason = ("seed '{0}' ({1} profile folder(s), {2} saved-game package(s)); check a used slot first: python tools/save_records.py verify <seed>" -f $Seed, $profiles.Count, $pkgs.Count) }
}

# ---------------------------------------------------------------------------
# iter-464 bgruns: THE HARNESS KEYBOARD CHANNEL (kernel/host_automation.hpp).
# The user works on this PC while rolls run, so an automated run (SR_INSTANCE_ID
# set) keeps the game window in the BACKGROUND and reads its keyboard from a
# named file mapping instead of the focused window:
#     Local\SR_HARNESS_INPUT_<instance>, 512 SDL-scancode bytes at 0x40
#     (nonzero = held).
# The game maps those bytes through the SAME iter-450S table it applies to a
# physical keyboard, so a harness key reaches the guest exactly as that key
# would - with no SetForegroundWindow, no SendInput and no focus. The game also
# ignores the real keyboard and pads on an automated run (input isolation), so
# the user's own typing and controller never reach a background roll.
# Layout v1: +0 magic 'SRHI' 0x49485253, +4 version 1, +8 game PID,
#            +0x0C merges that read the channel, +0x10 last mapped pad mask.
# ---------------------------------------------------------------------------
$script:SR_HARNESS_MAGIC    = [uint32]0x49485253
$script:SR_HARNESS_VERSION  = [uint32]1
$script:SR_HARNESS_KEYS_OFF = 0x40
$script:SR_HARNESS_MAP_SIZE = 4096
# SDL scancodes (SDL_scancode.h) of the sr_drive key names. The names must be
# exactly SR_KEY_NAMES (tools/test_sr_drive_harness.ps1 checks both ways).
$script:SR_KEY_SCANCODES = @{
    ENTER = 40; RETURN = 40; ESCAPE = 41; BACK = 42
    UP = 82; DOWN = 81; LEFT = 80; RIGHT = 79
    LB = 20; RB = 8; A = 29; B = 27; X = 6; Y = 25; F9 = 66
    # iter-464 DT-2 dev-tool keys (docs/DEVTOOLS.md 6.3-6.4; kernel/stubs.cpp devHostKeyDown)
    F6 = 63; F7 = 64; F8 = 65
    KEY_W = 26; KEY_A = 4; KEY_S = 22; KEY_D = 7
    KEY_I = 12; KEY_J = 13; KEY_K = 14; KEY_L = 15; KEY_R = 21; KEY_F = 9
    # iter-465 modlib: the mod library keys
    SPACE = 44; TAB = 43; PAGEUP = 75; PAGEDOWN = 78; F12 = 69
}

# Mirrors kernel/host_automation.cpp harnessChannelName.
function Get-SrHarnessChannelName([string]$Instance) {
    $id = if ($Instance) { $Instance } else { 'default' }
    $id = [regex]::Replace($id, '[^A-Za-z0-9_.-]', '_')
    if ($id.Length -gt 64) { $id = $id.Substring(0, 64) }
    return ('Local\SR_HARNESS_INPUT_' + $id)
}

# Opens the channel of game process $ProcessId, retrying until the game has
# created it (it does so before its window appears) or $TimeoutMs passes. Only a
# channel stamped with THIS process's PID is accepted: a mapping left by another
# game process is refused, never written. Returns @{ ok; name; reason; mmf; view }.
function Open-SrHarnessInput([string]$Instance, [int]$ProcessId, [int]$TimeoutMs = 30000) {
    $name = Get-SrHarnessChannelName $Instance
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    $why = 'the game has not created it'
    while ($true) {
        $mmf = $null; $view = $null
        try {
            $mmf = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting($name,
                       [System.IO.MemoryMappedFiles.MemoryMappedFileRights]::ReadWrite)
            $view = $mmf.CreateViewAccessor(0, $script:SR_HARNESS_MAP_SIZE)
            $magic = $view.ReadUInt32(0); $ver = $view.ReadUInt32(4); $owner = $view.ReadUInt32(8)
            if ($magic -eq $script:SR_HARNESS_MAGIC -and $ver -eq $script:SR_HARNESS_VERSION -and
                $owner -eq [uint32]$ProcessId) {
                return @{ ok = $true; name = $name; reason = ''; mmf = $mmf; view = $view }
            }
            $why = ("magic=0x{0:X8} version={1} pid={2} (want SRHI v1 pid {3})" -f $magic, $ver, $owner, $ProcessId)
        } catch {
            $why = $_.Exception.Message
        }
        if ($view) { $view.Dispose() }
        if ($mmf) { $mmf.Dispose() }
        if ((Get-Date) -ge $deadline) {
            return @{ ok = $false; name = $name; reason = $why; mmf = $null; view = $null }
        }
        Start-Sleep -Milliseconds 200
    }
}

function Close-SrHarnessInput($Channel) {
    if (-not $Channel) { return }
    if ($Channel.view) { try { $Channel.view.Dispose() } catch {}; $Channel.view = $null }
    if ($Channel.mmf)  { try { $Channel.mmf.Dispose() } catch {}; $Channel.mmf = $null }
    $Channel.ok = $false
}

# Holds ($Down) or releases one key. Returns $true when the byte was written.
function Set-SrHarnessKey($Channel, [int]$ScanCode, [bool]$Down) {
    if (-not $Channel -or -not $Channel.ok -or -not $Channel.view) { return $false }
    if ($ScanCode -le 0 -or $ScanCode -ge 512) { return $false }
    $v = [byte]0
    if ($Down) { $v = [byte]1 }
    try {
        $Channel.view.Write([int64]($script:SR_HARNESS_KEYS_OFF + $ScanCode), $v)
        return $true
    } catch { return $false }
}

# Reads one key back: $true = held. A closed or unreadable channel reads as
# HELD, so a release can never be "proven" through a dead channel.
function Test-SrHarnessKeyDown($Channel, [int]$ScanCode) {
    if (-not $Channel -or -not $Channel.ok -or -not $Channel.view) { return $true }
    if ($ScanCode -le 0 -or $ScanCode -ge 512) { return $true }
    try { return ($Channel.view.ReadByte([int64]($script:SR_HARNESS_KEYS_OFF + $ScanCode)) -ne 0) }
    catch { return $true }
}

# The game's own book-keeping: merges that read the channel, last mapped mask.
function Get-SrHarnessStats($Channel) {
    if (-not $Channel -or -not $Channel.ok -or -not $Channel.view) { return $null }
    try { return @{ polls = $Channel.view.ReadUInt32(0x0C); lastMask = $Channel.view.ReadUInt32(0x10) } }
    catch { return $null }
}
