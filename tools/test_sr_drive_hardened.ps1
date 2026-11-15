# iter-455l scenario-hardening self-test, r3 (director's four-closure-defect
# rework). Exercises the SHIPPED scenario contract end-to-end:
#   - Assert-HardenedScenario against the shipped JSON and a table of
#     contract mutations (pinned values, not ranges)
#   - Invoke-HardenedDialog (the ONE shared body production also calls)
#     driven with the SHIPPED step objects and simulated probes (virtual
#     clock - no game, no window, no real input)
#   - Get-SrAliveReason (exit-first liveness, shared production/sim)
#   - sr_drive.ps1 -ValidateOnly live invocations: the real production
#     validation path, guaranteed to stop before any process/dir/env action
# Exit code = number of failed checks. ASCII only.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'sr_drive_core.ps1')

$script:pass = 0
$script:fail = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { $script:pass++ }
    else { $script:fail++; Write-Host "[DRIVE_TEST] FAIL: $what" }
}

$SCN_PATH = Join-Path $PSScriptRoot 'scenarios/frontend_navigate_hardened.json'
function Load-Scn { Get-Content -Raw $SCN_PATH | ConvertFrom-Json }

# ---------------------------------------------------------------------------
# Simulated probes over a virtual clock. Only Sleep and focusCostMs advance
# time, so waits are exact. KeyDown schedules the confirm line (per
# $DeliverOffsets) and, optionally, the runtime selector line (txnOnPulse/
# txnDelayMs). Alive routes through the PRODUCTION Get-SrAliveReason.
# ---------------------------------------------------------------------------
$CONF_PAT  = 'KBPAD_OBS.*mappedMask=0x1000'
$HINTB_LINE = "[APT_REQ] #15 path='../common/ButtonHints/XBox360/HintB'"
$XAM_LINE   = '[XAM] XamShowDeviceSelectorUI(user=0,pDev=89679AAC) -> device=1'
$BANNER_LINE = 'import 000001A0 XamShowDeviceSelectorUI'

function New-Sim {
    param($Events = @(), $DeliverOffsets = @{}, $Opts = @{})
    $sim = @{ now = [int64]0
              events = [System.Collections.ArrayList]::new()
              keyDowns = 0; keyUps = 0; focusCalls = 0; pumpSpiked = $false
              keyDownAt = [System.Collections.ArrayList]::new()
              keyUpAt = [System.Collections.ArrayList]::new()
              opts = $Opts; deliver = $DeliverOffsets }
    foreach ($e in $Events) { [void]$sim.events.Add($e) }
    $sim.P = @{
        NowMs   = { $sim.now }.GetNewClosure()
        Sleep   = { param($ms) if ($ms -gt 0) { $sim.now += [int64]$ms } }.GetNewClosure()
        CountOf = { param($pat)
                    # pumpSpikeAt/pumpSpikeCostMs: the FIRST sample at/after
                    # the trigger consumes pump time itself (production
                    # Pump-Log can do this) - the r4 pump-cost fixtures.
                    if ($sim.opts.pumpSpikeAt -and (-not $sim.pumpSpiked) -and $sim.now -ge [int64]$sim.opts.pumpSpikeAt -and
                        ((-not $sim.opts.pumpSpikePat) -or ($pat -eq $sim.opts.pumpSpikePat))) {
                        $sim.pumpSpiked = $true
                        $sim.now += [int64]$sim.opts.pumpSpikeCostMs
                    }
                    @($sim.events | Where-Object { $_.at -le $sim.now -and $_.line -match $pat }).Count }.GetNewClosure()
        HitOf   = { param($pat)
                    $m = @($sim.events | Where-Object { $_.at -le $sim.now -and $_.line -match $pat })
                    if ($m.Count -gt 0) { [string]$m[-1].line } else { $null } }.GetNewClosure()
        Alive   = { Get-SrAliveReason ([bool]($sim.opts.exitAt -and $sim.now -ge [int64]$sim.opts.exitAt)) 1 `
                                      (-not ($sim.opts.deadAt -and $sim.now -ge [int64]$sim.opts.deadAt)) 123 }.GetNewClosure()
        Focus   = { $sim.focusCalls++
                    if ($sim.opts.focusCostMs) { $sim.now += [int64]$sim.opts.focusCostMs }
                    -not ($sim.opts.focusFailAtCall -and $sim.focusCalls -eq [int]$sim.opts.focusFailAtCall) }.GetNewClosure()
        KeyDown = { $sim.keyDowns++
                    [void]$sim.keyDownAt.Add($sim.now)
                    $i = $sim.keyDowns
                    if ($sim.deliver.ContainsKey($i)) {
                        [void]$sim.events.Add(@{ at = $sim.now + [int64]$sim.deliver[$i]; line = 'KBPAD_OBS pad=0 mappedMask=0x1000' })
                    }
                    if ($sim.opts.txnOnPulse -and $i -eq [int]$sim.opts.txnOnPulse) {
                        $d = [int64]$(if ($sim.opts.txnDelayMs) { $sim.opts.txnDelayMs } else { 500 })
                        [void]$sim.events.Add(@{ at = $sim.now + $d; line = '[XAM] XamShowDeviceSelectorUI(user=0,pDev=89679AAC) -> device=1' })
                    } }.GetNewClosure()
        KeyUp   = { $sim.keyUps++; [void]$sim.keyUpAt.Add($sim.now) }.GetNewClosure()
        Note    = { param($m) }
    }
    return $sim
}

# ---------------------------------------------------------------------------
# A. The pinned-contract validator: shipped scenario passes; every meaningful
#    field mutation is rejected (table-driven, director rework item 4).
# ---------------------------------------------------------------------------
$SCN = Load-Scn
$V = Assert-HardenedScenario $SCN
Check ([bool]$V.ok) "the shipped hardened scenario passes the pinned-contract validator ($($V.reason))"
$AWAIT  = @($SCN.steps)[$V.awaitIdx]
$SETTLE = @($SCN.steps)[$V.settleIdx]
$CLEAR  = @($SCN.steps)[$V.clearIdx]
Check ($HINTB_LINE -match $AWAIT.await) "the shipped HintB regex matches the real APT_REQ line shape"
Check ($XAM_LINE -match $CLEAR.expect) "the shipped selector regex matches the runtime invocation line"
Check (-not ($BANNER_LINE -match $CLEAR.expect)) "the shipped selector regex does NOT match the import banner"

$ai = $V.awaitIdx; $si = $V.settleIdx; $ci = $V.clearIdx
$CONTRACT_MUTS = @(
    @{ d = 'HintB await step removed';            m = { param($x) $x.steps = @($x.steps | Where-Object { $_.name -ne 'await-ui-tail' }) } },
    @{ d = 'await regex broadened (HintBogus)';   m = { param($x) (@($x.steps)[$ai]).await = (@($x.steps)[$ai]).await -replace 'HintB', 'HintBogus' } },
    @{ d = 'await step renamed';                  m = { param($x) (@($x.steps)[$ai]).name = 'await-ui-tail2' } },
    @{ d = 'settle step removed';                 m = { param($x) $x.steps = @($x.steps | Where-Object { $_.name -ne 'settle-dialog' }) } },
    @{ d = 'settle step renamed';                 m = { param($x) (@($x.steps)[$si]).name = 'settle-dialog2' } },
    @{ d = 'singleDeadline removed';              m = { param($x) (@($x.steps)[$ci]).singleDeadline = $false } },
    @{ d = 'clear step renamed';                  m = { param($x) (@($x.steps)[$ci]).name = 'clear-dialog2' } },
    @{ d = 'selector broadened to the bare name'; m = { param($x) (@($x.steps)[$ci]).expect = 'XamShowDeviceSelectorUI' } },
    @{ d = 'holdUntilConfirmed dropped';          m = { param($x) (@($x.steps)[$ci]).holdUntilConfirmed = $false } },
    @{ d = 'key changed to B';                    m = { param($x) (@($x.steps)[$ci]).key = 'B' } },
    @{ d = 'expectMs off-pin (60000)';            m = { param($x) (@($x.steps)[$ci]).expectMs = 60000 } },
    @{ d = 'maxPulses off-pin (1)';               m = { param($x) (@($x.steps)[$ci]).maxPulses = 1 } },
    @{ d = 'holdMs off-pin (250)';                m = { param($x) (@($x.steps)[$ci]).holdMs = 250 } },
    @{ d = 'gapMs off-pin (12000)';               m = { param($x) (@($x.steps)[$ci]).gapMs = 12000 } },
    @{ d = 'await timeoutMs off-pin (60000)';     m = { param($x) (@($x.steps)[$ai]).timeoutMs = 60000 } },
    @{ d = 'settle dwellMs off-pin (500)';        m = { param($x) (@($x.steps)[$si]).dwellMs = 500 } }
)
foreach ($mu in $CONTRACT_MUTS) {
    $x = Load-Scn
    & $mu.m $x
    Check (-not (Assert-HardenedScenario $x).ok) ("contract mutation REJECTED: " + $mu.d)
}

# ---------------------------------------------------------------------------
# A2. Exit-first liveness (director rework item 3) - the shared function.
# ---------------------------------------------------------------------------
Check ((Get-SrAliveReason $true 1 $false 123) -match 'process exited') "alive order: exit + HWND loss together -> 'process exited' wins"
Check ((Get-SrAliveReason $false 0 $false 123) -match 'window vanished') "alive order: HWND loss alone -> 'window vanished'"
Check ($null -eq (Get-SrAliveReason $false 0 $true 123)) "alive order: healthy -> null"

# ---------------------------------------------------------------------------
# B. The shared body with the SHIPPED steps (await 180000 / settle 8000 /
#    clear: 12 pulses, hold 4000, gap 5000, expect 150000).
# ---------------------------------------------------------------------------

# B1. ABSENT readiness: fails in the readiness phase after exactly ONE
#     bounded window, with ZERO key input sent.
$sim = New-Sim
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CLEAR $CONF_PAT $sim.P
Check ((-not $r.ok) -and (-not $r.fatal) -and $r.phase -eq 'readiness') "absent readiness fails in the readiness phase ($($r.reason))"
Check ($sim.keyDowns -eq 0) "absent readiness: ZERO key input was sent (got $($sim.keyDowns))"
Check ($r.elapsedMs -eq [int]$AWAIT.timeoutMs) "absent readiness: exactly one bounded await window (got $($r.elapsedMs))"

# B2. DELAYED readiness (HintB at 60 s): the first key-down happens only
#     after readiness + the full settle - never before.
$sim = New-Sim -Events @(@{ at = 60000; line = $HINTB_LINE }) -DeliverOffsets @{ 1 = 200 } -Opts @{ txnOnPulse = 1; txnDelayMs = 500 }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CLEAR $CONF_PAT $sim.P
Check ([bool]$r.ok) "delayed readiness: the dialog clears ($($r.reason))"
Check ($sim.keyDowns -ge 1 -and [int64]$sim.keyDownAt[0] -ge (60000 + [int]$SETTLE.dwellMs)) `
      "delayed readiness: first key-down only after readiness+settle (at $($sim.keyDownAt[0]))"

# B3. Delivered-without-transition: all 12 pulses delivered, no selector.
#     Honest failure at ONE absolute deadline - delivery is not transition.
$del = @{}; 1..12 | ForEach-Object { $del[$_] = 200 }
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }) -DeliverOffsets $del
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CLEAR $CONF_PAT $sim.P
Check ((-not $r.ok) -and $r.delivered -eq 12 -and (-not $r.fatal)) "delivered-without-transition: 12 delivered, still fails ($($r.delivered))"
$oneDeadline = [int]$SETTLE.dwellMs + [int]$CLEAR.expectMs
Check ($r.elapsedMs -eq $oneDeadline) "delivered-without-transition: elapsed == settle + ONE deadline (got $($r.elapsedMs), want $oneDeadline)"

# B4. Transition ON the final pulse: seen within one polling quantum of the
#     line landing (out-of-pulses watch).
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }) -DeliverOffsets $del -Opts @{ txnOnPulse = 12; txnDelayMs = 500 }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CLEAR $CONF_PAT $sim.P
Check ($r.ok -and $r.pulses -eq 12 -and $r.elapsedMs -le 65800) "transition on the final pulse, within one quantum (pulses=$($r.pulses), elapsed $($r.elapsedMs))"

# B5. TRANSITION LAG, hold: the selector lands mid-hold (pulse 3) - the body
#     returns within one quantum, through finally (key released).
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }, @{ at = 40000; line = $XAM_LINE }) -DeliverOffsets @{}
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CLEAR $CONF_PAT $sim.P
Check ($r.ok -and $r.elapsedMs -le 40100) "transition mid-hold: seen within one quantum (got $($r.elapsedMs) ms)"
Check ($sim.keyUps -eq $sim.keyDowns) "transition mid-hold: the held key was released (downs=$($sim.keyDowns) ups=$($sim.keyUps))"

# B5b. TRANSITION LAG, confirmation wait: selector at 13 s (inside pulse 1's
#      post-release confirm wait) - seen within one quantum.
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }, @{ at = 13000; line = $XAM_LINE }) -DeliverOffsets @{}
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CLEAR $CONF_PAT $sim.P
Check ($r.ok -and $r.elapsedMs -le 13100) "transition in the confirm wait: within one quantum (got $($r.elapsedMs) ms)"
Check ($sim.keyUps -eq $sim.keyDowns) "transition in the confirm wait: no key left down"

# B5c. TRANSITION LAG, gap: pulse 1 delivers fast, selector lands inside the
#      inter-pulse gap - seen within one quantum.
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }, @{ at = 9000; line = $XAM_LINE }) -DeliverOffsets @{ 1 = 200 }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CLEAR $CONF_PAT $sim.P
Check ($r.ok -and $r.elapsedMs -le 9100) "transition in the gap: within one quantum (got $($r.elapsedMs) ms)"

# B6. ABSOLUTE deadline, part 1 - expiry lands inside the first hold: the key
#     is released AT the deadline and the step fails. One overridden field
#     (expectMs=3000, documented); every other field is the shipped object's.
$c = Load-Scn; $CC = @($c.steps)[$V.clearIdx]; $CC.expectMs = 3000
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }) -DeliverOffsets @{}
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CC $CONF_PAT $sim.P
$edl = [int64]([int]$SETTLE.dwellMs + 3000)
Check ((-not $r.ok) -and $r.reason -eq 'single-deadline expired') "expiry mid-hold fails the step ($($r.reason))"
Check ($sim.keyUps -eq 1 -and [int64]$sim.keyUpAt[0] -le $edl) "expiry mid-hold: key released AT the deadline (up at $($sim.keyUpAt[0]), deadline $edl)"
Check ($r.elapsedMs -eq $edl) "expiry mid-hold: elapsed == the absolute deadline (got $($r.elapsedMs))"

# B7. ABSOLUTE deadline, part 2 - a transition FIRST OBSERVED at-or-after
#     expiry never succeeds. The line arises at 17950 ms, between the last
#     pre-deadline quantum (17900) and the deadline (18000): the first check
#     that could see it runs AT expiry, and expiry must win.
$c = Load-Scn; $CC = @($c.steps)[$V.clearIdx]; $CC.expectMs = 10000
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }, @{ at = 17950; line = $XAM_LINE }) -DeliverOffsets @{}
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CC $CONF_PAT $sim.P
Check ((-not $r.ok) -and $r.reason -eq 'single-deadline expired') "a transition first observed at expiry is REJECTED ($($r.reason), elapsed $($r.elapsedMs))"

# B8. FATAL: plain focus failure (window alive) - immediate, real reason,
#     zero keys sent.
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }) -Opts @{ focusFailAtCall = 1 }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CLEAR $CONF_PAT $sim.P
Check ((-not $r.ok) -and $r.fatal -and $r.reason -match 'focus') "focus failure is FATAL with its real reason ($($r.reason))"
Check ($sim.keyDowns -eq 0 -and $r.elapsedMs -le ([int]$SETTLE.dwellMs + 1000)) "focus failure: immediate (elapsed $($r.elapsedMs)), no key sent"

# B9. FATAL: window death DURING a hold - immediate, real reason, key-up
#     still guaranteed.
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }) -Opts @{ deadAt = 10000 }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CLEAR $CONF_PAT $sim.P
Check ((-not $r.ok) -and $r.fatal -and $r.reason -match 'window vanished') "window death mid-hold is FATAL with its real reason ($($r.reason))"
Check ($sim.keyDowns -eq 1 -and $sim.keyUps -eq 1) "window death mid-hold: key-up still guaranteed"
Check ($r.elapsedMs -eq 10000) "window death: reported AT the event, not at the deadline (got $($r.elapsedMs))"

# B10. FATAL: process exit during the post-release confirm wait.
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }) -Opts @{ exitAt = 13000 }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CLEAR $CONF_PAT $sim.P
Check ((-not $r.ok) -and $r.fatal -and $r.reason -match 'process exited') "process exit mid-wait is FATAL with its real reason ($($r.reason))"
Check ($r.elapsedMs -eq 13000 -and $sim.keyUps -eq $sim.keyDowns) "process exit: reported AT the event; key-up guaranteed"

# B10b. FATAL, combined: process exit takes its HWND with it in the same
#       instant - the reason must be 'process exited', never 'window
#       vanished' (director rework item 3).
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }) -Opts @{ exitAt = 10000; deadAt = 10000 }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CLEAR $CONF_PAT $sim.P
Check ($r.fatal -and $r.reason -match 'process exited') "exit + HWND loss together -> 'process exited' ($($r.reason))"
Check ($sim.keyUps -eq $sim.keyDowns) "exit + HWND loss: key-up guaranteed"

# B11. FOCUS OVERRUN (director rework item 1): focus consumes the whole
#      remaining budget - ZERO key-downs permitted afterward.
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }) -Opts @{ focusCostMs = 151000 }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CLEAR $CONF_PAT $sim.P
Check ((-not $r.ok) -and $r.reason -eq 'single-deadline expired') "focus overrun: the step expires honestly ($($r.reason))"
Check ($sim.keyDowns -eq 0) "focus overrun: ZERO key-downs after the budget was consumed (got $($sim.keyDowns))"

# B12. FATAL during focus: the process dies while Focus-Game is working - the
#      reason is the death, not 'could not focus'.
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }) -Opts @{ focusFailAtCall = 1; focusCostMs = 2000; exitAt = 9000 }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CLEAR $CONF_PAT $sim.P
Check ($r.fatal -and $r.reason -match 'process exited') "death during focus carries the REAL reason ($($r.reason))"
Check ($sim.keyDowns -eq 0) "death during focus: no key sent"

# B12b. SLOW FAILED FOCUS consuming the budget (r4 defect 3): the result is
#       deadline expiry, not 'could not focus' - and still zero keys.
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }) -Opts @{ focusFailAtCall = 1; focusCostMs = 151000 }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CLEAR $CONF_PAT $sim.P
Check ((-not $r.ok) -and (-not $r.fatal) -and $r.reason -eq 'single-deadline expired') "slow FAILED focus past the deadline is EXPIRY, not a focus error ($($r.reason))"
Check ($sim.keyDowns -eq 0) "slow failed focus: no key sent"

# B12c. Transition DURING a failed focus (r4 defect 3): the selector appeared
#       while focus was failing - recognized as success before any focus
#       error is reported.
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }, @{ at = 9000; line = $XAM_LINE }) -Opts @{ focusFailAtCall = 1; focusCostMs = 2000 }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CLEAR $CONF_PAT $sim.P
Check ($r.ok -and $r.reason -eq 'observed') "a selector during a FAILED focus wins over the focus error ($($r.reason))"
Check ($sim.keyDowns -eq 0) "transition during failed focus: no key was needed or sent"

# B14. PUMP-COST trio (r4 defect 2): the log sample itself consumes time
#      (production CountOf runs Pump-Log). A result first EXPOSED by a pump
#      that crossed a deadline must not count.
# B14a. Selector exposed after the engine deadline -> expiry, not success.
$c = Load-Scn; $CC = @($c.steps)[$V.clearIdx]; $CC.expectMs = 10000
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }, @{ at = 17500; line = $XAM_LINE }) -DeliverOffsets @{} -Opts @{ pumpSpikeAt = 17000; pumpSpikeCostMs = 1000 }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CC $CONF_PAT $sim.P
Check ((-not $r.ok) -and $r.reason -eq 'single-deadline expired') "pump-exposed selector after the deadline is EXPIRY ($($r.reason))"
# B14b. HintB exposed after the readiness deadline -> readiness fails, no input.
$sim = New-Sim -Events @(@{ at = 180500; line = $HINTB_LINE }) -Opts @{ pumpSpikeAt = 179800; pumpSpikeCostMs = 1200 }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CLEAR $CONF_PAT $sim.P
Check ((-not $r.ok) -and $r.phase -eq 'readiness' -and (-not $r.fatal)) "pump-exposed HintB after the readiness deadline FAILS readiness ($($r.reason))"
Check ($sim.keyDowns -eq 0) "pump-exposed late HintB: still zero input"
# B14c. Confirm exposed after expiry -> no delivery counted, no further key.
#       The spike is pinned to the CONFIRM sample (pumpSpikePat) so the
#       confirm site's own post-sample rechecks are what is exercised.
$c = Load-Scn; $CC = @($c.steps)[$V.clearIdx]; $CC.expectMs = 10000
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }) -DeliverOffsets @{ 1 = 9500 } -Opts @{ pumpSpikeAt = 17000; pumpSpikeCostMs = 1000; pumpSpikePat = $CONF_PAT }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CC $CONF_PAT $sim.P
Check ((-not $r.ok) -and $r.delivered -eq 0) "pump-exposed confirm after expiry counts NO delivery (delivered=$($r.delivered))"
Check ($sim.keyDowns -eq 1) "pump-exposed confirm after expiry: no further key was sent"

# B15. THE PRE-KEY CONFIRMATION MARK (r5 blocker): the confirmation BASELINE
#      is the FOURTH pumping sample. It runs before the first key-down of a
#      pulse (the loop-top and post-focus watches never sample the confirm
#      pattern), so pinning the spike to the confirm pattern with an early
#      trigger lands it exactly on the mark.
# B15a. Mark pump crosses the deadline -> expiry, ZERO key-downs.
$c = Load-Scn; $CC = @($c.steps)[$V.clearIdx]; $CC.expectMs = 10000
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }) -DeliverOffsets @{} -Opts @{ pumpSpikeAt = 1; pumpSpikeCostMs = 11000; pumpSpikePat = $CONF_PAT }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CC $CONF_PAT $sim.P
Check ((-not $r.ok) -and $r.reason -eq 'single-deadline expired') "mark pump crossing the deadline is EXPIRY ($($r.reason))"
Check ($sim.keyDowns -eq 0) "mark pump crossing the deadline: ZERO key-downs (got $($sim.keyDowns))"
# B15b. Selector becomes visible during the mark pump -> success, ZERO keys.
$c = Load-Scn; $CC = @($c.steps)[$V.clearIdx]; $CC.expectMs = 10000
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }, @{ at = 17000; line = $XAM_LINE }) -DeliverOffsets @{} -Opts @{ pumpSpikeAt = 1; pumpSpikeCostMs = 9500; pumpSpikePat = $CONF_PAT }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CC $CONF_PAT $sim.P
Check ($r.ok -and $r.reason -eq 'observed') "selector exposed during the mark pump is SUCCESS ($($r.reason))"
Check ($sim.keyDowns -eq 0) "selector during the mark pump: ZERO key-downs (got $($sim.keyDowns))"
# B15c. Process death during the mark pump -> fatal with the real reason,
#       ZERO keys.
$c = Load-Scn; $CC = @($c.steps)[$V.clearIdx]; $CC.expectMs = 10000
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }) -DeliverOffsets @{} -Opts @{ pumpSpikeAt = 1; pumpSpikeCostMs = 9500; pumpSpikePat = $CONF_PAT; exitAt = 16000 }
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CC $CONF_PAT $sim.P
Check ($r.fatal -and $r.reason -match 'process exited') "death during the mark pump carries the REAL reason ($($r.reason))"
Check ($sim.keyDowns -eq 0) "death during the mark pump: ZERO key-downs (got $($sim.keyDowns))"

# B13. The banner cannot satisfy the engine - by regex (shipped step) AND by
#      the production Test-RuntimeSelectorLine refusal (broadened step).
Check (-not (Test-RuntimeSelectorLine $BANNER_LINE)) "Test-RuntimeSelectorLine refuses the import banner"
Check (Test-RuntimeSelectorLine $XAM_LINE) "Test-RuntimeSelectorLine accepts the runtime invocation"
$c = Load-Scn; $CC = @($c.steps)[$V.clearIdx]; $CC.expect = 'XamShowDeviceSelectorUI'; $CC.expectMs = 60000
$sim = New-Sim -Events @(@{ at = 0; line = $HINTB_LINE }, @{ at = 9000; line = $BANNER_LINE }) -DeliverOffsets @{}
$r = Invoke-HardenedDialog $AWAIT $SETTLE $CC $CONF_PAT $sim.P
Check (-not $r.ok) "even a BROADENED expectation cannot promote the banner - the watch re-checks the line ($($r.reason))"

# ---------------------------------------------------------------------------
# C. PRODUCTION path: sr_drive.ps1 -ValidateOnly runs the real validation and
#    exits before any process/dir/env action (exit 0 valid / 2 invalid).
# ---------------------------------------------------------------------------
$drive = Join-Path $PSScriptRoot 'sr_drive.ps1'
$tmp = Join-Path $env:TEMP ("sr_drive_hardened_test_" + [IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $noOut = Join-Path $tmp 'never_created_out'
    function Invoke-ValidateOnly([string]$scnPath) {
        # cmd owns the redirection: under EAP=Stop, a PS-side stderr redirect
        # of a native child wraps lines into terminating NativeCommandErrors.
        cmd /c ("powershell -NoProfile -ExecutionPolicy Bypass -File ""$drive"" -Scenario ""$scnPath"" -Out ""$noOut"" -ValidateOnly >nul 2>&1")
        return $LASTEXITCODE
    }
    Check ((Invoke-ValidateOnly $SCN_PATH) -eq 0) "production -ValidateOnly accepts the shipped hardened scenario"
    Check (-not (Test-Path $noOut)) "-ValidateOnly touched no artifact directory"

    $legacy = Join-Path $PSScriptRoot 'scenarios/frontend_navigate.json'
    Check ((Invoke-ValidateOnly $legacy) -eq 0) "production -ValidateOnly leaves the legacy (non-hardened) scenario alone"

    $m = Load-Scn; (@($m.steps)[$V.clearIdx]).expect = 'XamShowDeviceSelectorUI'
    $p1 = Join-Path $tmp 'broadened.json'; $m | ConvertTo-Json -Depth 8 | Set-Content -Path $p1 -Encoding ascii
    Check ((Invoke-ValidateOnly $p1) -eq 2) "production rejects the BROADENED selector with exit 2, before any process"

    $m = Load-Scn; $m.steps = @($m.steps | Where-Object { $_.name -ne 'await-ui-tail' })
    $p2 = Join-Path $tmp 'noawait.json'; $m | ConvertTo-Json -Depth 8 | Set-Content -Path $p2 -Encoding ascii
    Check ((Invoke-ValidateOnly $p2) -eq 2) "production rejects the readiness-stripped scenario with exit 2"

    $m = Load-Scn; (@($m.steps)[$V.clearIdx]).singleDeadline = $false
    $p3 = Join-Path $tmp 'nodeadline.json'; $m | ConvertTo-Json -Depth 8 | Set-Content -Path $p3 -Encoding ascii
    Check ((Invoke-ValidateOnly $p3) -eq 2) "production rejects the singleDeadline-stripped scenario with exit 2"

    $m = Load-Scn; (@($m.steps)[$V.clearIdx]).key = 'B'
    $p4 = Join-Path $tmp 'keyb.json'; $m | ConvertTo-Json -Depth 8 | Set-Content -Path $p4 -Encoding ascii
    Check ((Invoke-ValidateOnly $p4) -eq 2) "production rejects the off-contract key with exit 2"

    # r4 defect 1: -ValidateOnly must run BEFORE Add-Type or any other
    # initialization, so it works even where %TEMP% is unwritable (the
    # condition that broke it in the director's review environment).
    function Invoke-ValidateOnlyBadTemp([string]$scnPath) {
        cmd /c ("set TEMP=C:\sr_no_such_temp_dir_455l&& set TMP=C:\sr_no_such_temp_dir_455l&& powershell -NoProfile -ExecutionPolicy Bypass -File ""$drive"" -Scenario ""$scnPath"" -Out ""$noOut"" -ValidateOnly >nul 2>&1")
        return $LASTEXITCODE
    }
    Check ((Invoke-ValidateOnlyBadTemp $SCN_PATH) -eq 0) "-ValidateOnly accepts the shipped scenario even with an unwritable TEMP"
    Check ((Invoke-ValidateOnlyBadTemp $p1) -eq 2) "-ValidateOnly rejects the broadened scenario (exit 2) even with an unwritable TEMP"
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

# D. Wiring: validation precedes every process/artifact action in the source,
#    the step loop calls the ONE shared body, and liveness is the shared
#    exit-first function (mutation tripwires).
$src = Get-Content -Raw $drive
$iVal = $src.IndexOf('Assert-HardenedScenario')
Check ($iVal -gt 0 -and $iVal -lt $src.IndexOf('Lock-SrArtifactRoot') -and $iVal -lt $src.IndexOf('Start-Process')) `
      "sr_drive.ps1 validates BEFORE artifact locking and process launch"
Check ($src.IndexOf('if ($ValidateOnly)') -gt 0 -and $src.IndexOf('if ($ValidateOnly)') -lt $src.IndexOf('Add-Type -TypeDefinition')) `
      "the -ValidateOnly exit precedes the Add-Type statement and all other initialization"
Check ($src.Contains('Invoke-HardenedDialog')) "sr_drive.ps1 calls the shared hardened-dialog body"
Check ($src.Contains('Get-SrAliveReason')) "sr_drive.ps1's Alive probe uses the shared exit-first liveness"
Check (-not $src.Contains('Invoke-PulseEngine')) "the superseded r1 engine is gone from production"

$total = $script:pass + $script:fail
Write-Host "[DRIVE_TEST] $($script:pass)/$total passed"
exit $script:fail
