# test_sr_drive_wiring.ps1 - PRODUCTION WIRING, not helper logic.
#
# WHY IT EXISTS: an anchor-based edit sliced sr_drive.ps1 from Assert-KeyReleased
# to Release-AllKeys without checking what sat between them, and deleted the
# Wait-SustainedBlack wrapper. The CALL SITE survived. Every helper test still
# passed - 20/20 on the gate, 85/85 on the driver, 99/99 on artifacts - because
# they drive the core bodies directly and never the production adapters. The
# acquisition would have died at observe-gameplay instead of applying the gate.
#
# So this suite asserts the wiring itself, by AST:
#   - every command sr_drive.ps1 invokes is either defined locally, dot-sourced
#     from sr_drive_core.ps1, or a real cmdlet/executable;
#   - the functions the gate and the release path depend on exist and are
#     called;
#   - each is defined exactly ONCE (a second definition silently wins).
#
# Exit code = number of failed checks. ASCII only.

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)
$pass = 0; $fail = 0
function Check([bool]$ok, [string]$name) {
    if ($ok) { $script:pass++; Write-Host "  ok  $name" }
    else { $script:fail++; Write-Host "FAIL: $name" }
}

function Get-Ast([string]$path) {
    $t = $null; $e = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile(
        (Resolve-Path $path).Path, [ref]$t, [ref]$e)
    if ($e -and $e.Count) { throw "parse errors in ${path}: $($e[0].Message)" }
    return $ast
}

$driveP = 'tools/sr_drive.ps1'
$coreP  = 'tools/sr_drive_core.ps1'
# sr_drive.ps1 dot-sources BOTH core files; the wiring check has to know about
# every one of them or it reports false unresolved symbols. Discovered by this
# test's own first run, which flagged the six Sr-artifact functions.
$artP   = 'tools/sr_artifacts.ps1'
$bgP    = 'tools/bg_window.ps1'     # iter-464 bgruns: Start-SrGameProcess
$quitP  = 'tools/sr_quit.ps1'       # tools/graceful-quit: Invoke-SrGracefulQuit & co.
$drive  = Get-Ast $driveP
$core   = Get-Ast $coreP
$arts   = Get-Ast $artP
$bgw    = Get-Ast $bgP
$quit   = Get-Ast $quitP

function Get-Defs($ast) {
    $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true) |
        ForEach-Object { $_.Name }
}
function Get-Calls($ast) {
    $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] }, $true) |
        ForEach-Object { $_.GetCommandName() } | Where-Object { $_ }
}

$defsDrive = @(Get-Defs $drive)
$defsCore  = @(Get-Defs $core)
$defsArts  = @(Get-Defs $arts)
$defsBg    = @(Get-Defs $bgw)
$defsQuit  = @(Get-Defs $quit)
$defs      = @($defsDrive + $defsCore + $defsArts + $defsBg + $defsQuit)
$calls     = @(Get-Calls $drive) + @(Get-Calls $core) + @(Get-Calls $arts) + @(Get-Calls $bgw) + @(Get-Calls $quit)

Check ($defsDrive.Count -gt 0 -and $defsCore.Count -gt 0 -and $defsArts.Count -gt 0) `
    "W1: all three dot-sourced scripts parse and define functions (drive=$($defsDrive.Count) core=$($defsCore.Count) artifacts=$($defsArts.Count))"

# --- W2: THE DEFECT. Every Verb-Noun call resolves to a definition or a real
# command. This is the check whose absence let a deleted wrapper ship.
$unresolved = @()
foreach ($c in ($calls | Sort-Object -Unique)) {
    if ($defs -contains $c) { continue }
    if (Get-Command $c -ErrorAction SilentlyContinue) { continue }
    $unresolved += $c
}
Check ($unresolved.Count -eq 0) `
    ("W2: every command invoked is defined or resolvable" + $(if ($unresolved.Count) { " -- UNRESOLVED: $($unresolved -join ', ')" } else { "" }))

# --- W3: no function is defined twice (a second definition silently wins)
$dupes = @($defs | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })
Check ($dupes.Count -eq 0) `
    ("W3: no function is defined more than once" + $(if ($dupes.Count) { " -- DUPLICATES: $($dupes -join ', ')" } else { "" }))

# --- W4/W5: the two chains this acquisition depends on are wired end to end
foreach ($fn in @('Wait-SustainedBlack', 'Invoke-SustainedBlackGate', 'Test-FrameExactBlack')) {
    Check (($defs -contains $fn) -and ($calls -contains $fn)) "W4: black-gate chain - '$fn' is BOTH defined and called"
}
foreach ($fn in @('Assert-KeyReleased', 'Invoke-VerifiedRelease', 'Send-Key')) {
    Check (($defs -contains $fn) -and ($calls -contains $fn)) "W5: release chain - '$fn' is BOTH defined and called"
}

# --- W6: the gate is actually reachable from the step loop
$src = Get-Content $driveP -Raw
Check ($src -match 'requireBlackMs') "W6: the step loop reads requireBlackMs, so a gated scenario can arm it"
# iter-458 (Astra 038): the visible gate, same shape
foreach ($fn in @('Wait-SustainedVisible', 'Invoke-SustainedVisibleGate')) {
    Check (($defs -contains $fn) -and ($calls -contains $fn)) "W6b: visible-gate chain - '$fn' is BOTH defined and called"
}
Check ($src -match 'if \(\$s\.requireVisibleMs\) \{') "W6c: the step loop is guarded on requireVisibleMs exactly, so a gated scenario can arm it"

# --- W7: the verdict line is emitted ONLY by the finalization body. A literal
# in the driver would mean some path can announce a verdict before cleanup and
# health adjudication have run - which is the defect this replaced. Structural
# on purpose; the BEHAVIOUR is covered by test_sr_drive_finalization.ps1.
# Comments may DISCUSS the verdict; only executable code must not emit it. The
# check strips comment lines rather than being weakened to a substring match.
$srcCode = ($src -split "`n" | Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
Check (-not ($srcCode -match 'ALL STEPS PASSED')) `
    "W7: no verdict literal in sr_drive.ps1 CODE - only Invoke-RunFinalization announces"

foreach ($fn in @('Invoke-CleanupAndFinalize', 'Invoke-ReleaseAllBody', 'New-BlackGateProbes', 'New-ProductionKeyProbes')) {
    Check (($defs -contains $fn) -and ($calls -contains $fn)) "W8: finalization chain - '$fn' is BOTH defined and called"
}

# --- W10 (iter-458): the dwell and the pre-finalization check are liveness-gated
foreach ($fn in @('Invoke-DwellBody', 'New-LivenessProbe', 'Invoke-PreFinalLiveness')) {
    Check (($defs -contains $fn) -and ((Get-Calls $drive) -contains $fn)) "W10: liveness chain - '$fn' is defined and called from sr_drive.ps1"
}

# --- W9: the held list is never cleared without re-adding what did not release
Check ($src -match 'foreach \(\$k in @\(\$fin\.remaining\)\)') `
    "W9: unreleased keys are re-added after clearing - bookkeeping is not discarded"

Write-Host "[WIRING_TEST] $pass/$($pass + $fail) passed"
exit $fail
