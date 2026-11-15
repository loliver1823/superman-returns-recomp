# run_recomp_x.ps1 - Launch K parallel SupermanReturns instances, then aggregate
# the menu-progress markers across all K logs into one summary.
#
# WHY: the menu chain is severely roll-dependent (DOGMA non-determinism, iter-242/244)
# - a CORRECT fix may not fire on a given boot. Running K instances per iteration and
# aggregating beats the roll: if ANY run hits DEF_EXEC/PARSE_TRACE, the gate works.
#
# The app already supports this: SR_INSTANCE_ID skips the zombie-killer and writes a
# per-instance log (superman_returns_<id>.log). Assets are read-only, so K coexist.
#
# Usage:
#   .\tools\run_recomp_x.ps1 -K 4 -TimeoutSeconds 90 `
#       -Gates "SR_FORCE_APTCTX=1,SR_DRIVE_APT_TICK=1,SR_DRIVE_ACTIVATE_UI=1,SR_DRIVE_LOADMOVIE=1,SR_DRIVE_APTPARSE=1"
#
# Emits a machine-readable JSON summary to tools\last_run_x.json (the analysis
# workflow / loop reads this) plus a human table to stdout.
# Known runtime failures (including a child THREAD_FATAL followed by process
# exit 0), missing logs, or failed scans return 6 after preserving the summary.
# NATIVE_*_BAD markers remain available for intentional negative controls.

param(
    [int]    $K               = 4,
    [int]    $TimeoutSeconds  = 90,
    [string] $Gates           = "",            # comma-separated NAME=VALUE pairs applied to every instance
    [string] $ExeName         = "SupermanReturns",
    [int]    $MinPresentEarly = 0,             # if >0, stop a run early once it reaches this present count
    [string] $ExePath         = "",            # optional: run an exe snapshot (tools\snapshot_exe.ps1) instead of build\
    [string] $ContentRoot     = "",            # optional: repo checkout to run IN (cwd: assets, data/, logs) - e.g. a
                                               # worktree's exe against the main checkout's "Superman Returns (USA)/"
    [switch] $GracefulQuit                     # tools/graceful-quit (TOOLKIT 1.8m): end each instance by asking it to quit
                                               # (terminal snapshot, tools/sr_quit.ps1); Stop-Process only as the fallback
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'bg_window.ps1')   # iter-464 bgruns: Start-SrGameProcess
. (Join-Path $PSScriptRoot 'sr_quit.ps1')     # tools/graceful-quit: Invoke-SrGracefulQuit (functions only)
$projectRoot = Join-Path $PSScriptRoot ".." | Resolve-Path
$buildDir    = Join-Path $projectRoot "build"
if ($ContentRoot) {
    if (-not (Test-Path -LiteralPath $ContentRoot -PathType Container)) { Write-Error "-ContentRoot not found: $ContentRoot"; exit 1 }
    $projectRoot = (Resolve-Path -LiteralPath $ContentRoot).Path
}
# iter-463 FIX: PowerShell variables are case-insensitive, so the old
# `$exePath = Join-Path $buildDir ...` here OVERWROTE the -ExePath parameter
# before it was read: every -ExePath roll silently ran build\SupermanReturns.exe
# (caught by burndown2 boot1: the log carried strings absent from the snapshot).
$requestedExe = $ExePath
$gameExePath  = Join-Path $buildDir ($ExeName + ".exe")
if ($requestedExe) {
    # A snapshot lets the next build link while this roll runs (the running exe
    # is locked). The child still starts in the repo root, so the content root
    # and the relative data paths resolve exactly as they do for build\.
    $cand = if ([System.IO.Path]::IsPathRooted($requestedExe)) { $requestedExe } else { Join-Path $projectRoot $requestedExe }
    if (-not (Test-Path -LiteralPath $cand -PathType Leaf)) { Write-Error "-ExePath not found: $cand"; exit 1 }
    $gameExePath = (Resolve-Path -LiteralPath $cand).Path
    $ExeName = [System.IO.Path]::GetFileNameWithoutExtension($gameExePath)
}

if (-not (Test-Path $gameExePath)) { Write-Error "Exe not found: $gameExePath (build first)"; exit 1 }
Write-Host ("executable: {0}  (cwd/content root: {1})" -f $gameExePath, $projectRoot)

# Apply shared gate env vars to THIS process so each child inherits them.
$gateMap = @{}
if ($Gates) {
    # iter-456n GUARD: gates are COMMA-separated. A semicolon-separated string
    # parses as ONE pair whose value swallows every later gate — and the run
    # still LOOKS healthy, because the first gate applies and its census line
    # prints. That is exactly how a validation roll was banked with no trace
    # recorder and no present dumps (the [GATES] line read TRACE=-, four
    # minutes in). Fail loudly instead of silently dropping evidence.
    if ($Gates -match ";") {
        Write-Error ("SEMICOLON in -Gates: gates are COMMA-separated NAME=VALUE " +
                     "pairs. A ';' silently swallows every gate after the first. " +
                     "Offending string: $Gates")
        exit 1
    }
    foreach ($pair in $Gates.Split(",")) {
        $kv = $pair.Split("=", 2)
        if ($kv.Count -eq 2 -and $kv[0].Trim()) {
            $name = $kv[0].Trim(); $val = $kv[1].Trim()
            Set-Item -Path ("Env:" + $name) -Value $val
            $gateMap[$name] = $val
        }
    }
}

# iter-463 phase 2 (docs/DEVTOOLS_DESIGN.md 1.1): an automated roll never inherits a dev tool by
# accident - SR_INSTANCE_ID already makes the exe ignore the player's dev.txt, and SR_DEVTOOLS is
# pinned to 0 here unless the caller set it (env or -Gates). Any roll that does enable one is
# tagged: the runClass column below and verify_run.py refuse it as a natural result.
$prevDevtools = $env:SR_DEVTOOLS
if ($null -eq $env:SR_DEVTOOLS) { $env:SR_DEVTOOLS = "0" }

Write-Host "Launching $K instances, timeout ${TimeoutSeconds}s. Gates: $Gates"

# Kill ANY existing instances first. Stragglers from a prior batch (e.g. when the
# caller's timeout killed this script before its own kill loop ran) hold the
# per-instance logs open: Remove-Item below fails silently, the new instance
# can't truncate the log either, and the aggregate ends up reading the
# STRAGGLER's output — two batches were silently poisoned this way (iter-365f).
Get-Process -Name $ExeName -ErrorAction SilentlyContinue | ForEach-Object {
    try { Stop-Process -Id $_.Id -Force -ErrorAction Stop } catch {}
}
Start-Sleep -Milliseconds 300

# Clean old per-instance evidence so an early exit cannot inherit a previous
# run's successful graceful-quit receipt. Refuse if cleanup fails.
for ($i = 0; $i -lt $K; $i++) {
    $lp = Join-Path $projectRoot ("superman_returns_run{0}.log" -f $i)
    foreach ($stale in @($lp, [System.IO.Path]::ChangeExtension($lp, '.quit.json'))) {
        if (Test-Path -LiteralPath $stale) { Remove-Item -LiteralPath $stale -Force -ErrorAction Stop }
    }
}

# iter-463 SAVES ARE HERMETIC: each instance gets its OWN fresh save root under
# archive\content_rolls\<stamp>\run<i> (kept as evidence next to the logs), so a
# roll never reads the player's saves, a previous roll's save, or a sibling's
# package. A caller-supplied SR_CONTENT_DIR (env or -Gates) is a SEED: it is
# copied into every instance's root and never written itself.
$seedContentDir = $env:SR_CONTENT_DIR
$contentStamp = Get-Date -Format "yyyyMMdd-HHmmss"
$contentBase = Join-Path $projectRoot ("archive\content_rolls\" + $contentStamp)

# Launch K instances, each with a distinct SR_INSTANCE_ID -> distinct log file.
$procs = @()
for ($i = 0; $i -lt $K; $i++) {
    $env:SR_INSTANCE_ID = ("run{0}" -f $i)
    $contentDir = Join-Path $contentBase ("run{0}" -f $i)
    New-Item -ItemType Directory -Force $contentDir | Out-Null
    if ($seedContentDir -and (Test-Path -LiteralPath $seedContentDir)) {
        Copy-Item -Path (Join-Path $seedContentDir "*") -Destination $contentDir -Recurse -Force
    }
    $env:SR_CONTENT_DIR = $contentDir
    # iter-464 bgruns: no console window (Start-Process opened a Windows Terminal window
    # that took the foreground); the game backgrounds its own window (SR_INSTANCE_ID).
    $p = Start-SrGameProcess -FilePath $gameExePath -WorkingDirectory $projectRoot
    $null = $p.Handle   # Windows PowerShell 5.1: without it ExitCode reads back empty after the exit
    $procs +=[pscustomobject]@{ Index = $i; Proc = $p; Log = (Join-Path $projectRoot ("superman_returns_run{0}.log" -f $i)); ContentDir = $contentDir }
    Write-Host ("  run{0} PID {1} saves={2}{3}" -f $i, $p.Id, $contentDir, $(if ($seedContentDir) { " (seed $seedContentDir)" } else { " (empty)" }))
}
Remove-Item Env:SR_INSTANCE_ID -ErrorAction SilentlyContinue
if ($seedContentDir) { $env:SR_CONTENT_DIR = $seedContentDir } else { Remove-Item Env:SR_CONTENT_DIR -ErrorAction SilentlyContinue }
if ($null -eq $prevDevtools) { Remove-Item Env:SR_DEVTOOLS -ErrorAction SilentlyContinue } else { $env:SR_DEVTOOLS = $prevDevtools }

# Wait for the deadline (or all exited / early-present target met).
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
while ([DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 1000
    $alive = @($procs | Where-Object { -not $_.Proc.HasExited })
    if ($alive.Count -eq 0) { break }
    if ($MinPresentEarly -gt 0) {
        $allHit = $true
        foreach ($r in $procs) {
            $c = 0
            if (Test-Path $r.Log) {
                $m = Select-String -Path $r.Log -Pattern "\[D3D_DIAG\] present #(\d+)" -ErrorAction SilentlyContinue
                if ($m) { $c = ($m | ForEach-Object { [int]$_.Matches[-1].Groups[1].Value } | Measure-Object -Maximum).Maximum }
            }
            if ($c -lt $MinPresentEarly) { $allHit = $false; break }
        }
        if ($allHit) { Write-Host "All runs reached present>=$MinPresentEarly early."; break }
    }
}

# Kill any survivors (-GracefulQuit: ask each to quit first; it drains, prints its terminal snapshot and exits).
foreach ($r in $procs) {
    if (-not $r.Proc.HasExited) {
        if ($GracefulQuit -or $env:SR_DRIVE_GRACEFUL_QUIT -eq '1') {   # the switch, or the same env opt-in as sr_drive
            $gq = Invoke-SrGracefulQuit -Instance ("run{0}" -f $r.Index) -ProcessId $r.Proc.Id -P (New-SrQuitProbes $r.Proc)
            Add-SrQuitTerminalInfo $gq $r.Log   # native/quit-drain: the game's stranded record, from its log
            Write-Host ("  run{0} {1}" -f $r.Index, (Format-SrQuitResult $gq))
            # The receipt beside the log (superman_returns_run<i>.quit.json; tools/perf/terminal_snapshot.py finds it).
            $null = Write-SrQuitRecord ([System.IO.Path]::ChangeExtension($r.Log, '.quit.json')) $gq
        } else {
            Stop-Process -Id $r.Proc.Id -Force -ErrorAction SilentlyContinue
        }
    }
}
Start-Sleep -Milliseconds 400

# ---- Aggregate the menu-progress markers across all logs ----
function Get-SrRunLogHealth([string]$LogPath, [string]$Scanner = (Join-Path $PSScriptRoot 'perf/run_health.py')) {
    try {
        $python = if ($env:SR_PYTHON) { $env:SR_PYTHON } else { 'python' }
        $text = & $python $Scanner --log $LogPath
        $scanExit = $LASTEXITCODE
        $health = ($text -join "`n") | ConvertFrom-Json
        if ($health.schema -ne 'run-health/1' -or $health.healthy -isnot [bool] -or
            ($scanExit -eq 0) -ne $health.healthy -or $scanExit -notin @(0, 6)) {
            throw 'invalid log-health result'
        }
        return $health
    } catch {
        return [pscustomobject]@{ schema='run-health/1'; log=$LogPath; healthy=$false;
            verdict='UNKNOWN'; reason="log-health scan failed: $_"; scan_complete=$false }
    }
}

function Get-SrRunHealthExit($Runs) {
    if (@($Runs).Count -eq 0 -or @($Runs | Where-Object { $_.logHealth.healthy -ne $true }).Count) { return 6 }
    return 0
}

function Measure-Run([string]$logPath) {
    $r = [ordered]@{
        log        = $logPath
        logHealth  = Get-SrRunLogHealth $logPath
        exists     = (Test-Path $logPath)
        lines      = 0
        present    = 0     # max [D3D_DIAG] present #N  (north-star)
        defExec    = 0     # [DEF_EXEC] count  (movie def created -> THE goal)
        parser     = 0     # [PARSE_TRACE] *** sub_826AE6D0(PARSER) hits
        aptCtxOk   = 0     # [APTCTX] 0x829A17BC non-zero  (context established)
        loadMovie  = 0     # [APT_LOADMOVIE] count (FE root movie load; 1 = title dispatched)
        # ---- iter-358 HONEST GAUGES: read from the [GAUGE] census line, which
        # carries UNCAPPED atomic counters (kernel/natural_gauges.cpp). Never
        # count log lines here — the probes cap their verbose output, so a line
        # count reports the CAP as the measurement (the sampling trap, 6x).
        # What these replaced, and why each old one was a dead instrument:
        #   aptTick  <- [APT_TICK]      = splash_hooks INDEP loadMovie DRIVER (forced)
        #   defBuilt <- "DEF BUILT"     = [BLOB_INJECT] register chain (forced)
        #   movInst  <- "MOVIE INSTALLED" = [BLOB_INJECT] install (forced)
        #   feFrames <- [XMSG_7001A]    = iter-326 recalibration onto a premise
        #                                 iter-356 later REFUTED (0x7001A is the
        #                                 XMP music player, not an FE-state msg)
        # All four sat behind forcingAllowed() => 0 on every natural run BY
        # CONSTRUCTION, whatever the game did.
        aptPump    = 0     # sub_826D2718 Apt executor pump ticks (natural)
                           # (NOT 826D27C8 - that is entry+0xB0, a return address)
        clipCtor   = 0     # every type-12 clip ctor (bookkeeping included)
        movInst    = 0     # clip ctors with movieDef != NULL = REAL movie instances
        feFrames   = 0     # sub_821494C0 FE master tick, natural calls only
        aptReq     = 0     # sub_826D2690 load-by-name requests (uncapped)
        # ---- iter-305 NATURAL-BOOT signals (the drive-chain columns above
        # measure the RETIRED band-aid chain and read 0 on a healthy natural
        # boot; these measure the boot itself) ----
        walksRet   = 0     # E630_returned count (init walks completed; 4 = full init)
        walkDepth  = 0     # highest E928 call# that ENTERED E630 (how far the ladder got)
        schedWork  = 0     # [RTAM_SVC] worked>0 lines (scheduler doing real work)
        spinStorm  = 0     # max [FC108] call# (CS-helper storm; healthy boots stay tiny)
        lastInit   = ""    # last F2C8_vt7_pre module obj (the wedge module if walks stalled)
        init2Done  = 0     # "FSM released â€” Init2 complete" seen (post-init era reached)
        # iter-449q instrument audit: fsmPass ("[RTAM_SVC] FSM pass") was the
        # RETIRED forced FSM thread (SR_ALLOW_FORCING-gated — 0 by construction
        # on a natural roll) and cmd43's emitter was DELETED at iter-358f (its
        # semantic was a PIX cert-monitor command, never a draw). Both columns
        # removed. frameDisp counts the NATURAL observe on the same guest fn
        # fsmPass used to force (sub_8271F848 master dispatch,
        # alloc_overlap_watch.cpp) — 0 in every archived roll so far; nonzero
        # would be a new era.
        frameDisp  = 0     # [FRAME_DISPATCH] natural master-dispatch posts
        frameDumps = 0     # [FRAME_DUMP] shots written
        # ---- iter-442 S4.2 THE MOVIE ACTIVITY GATE ----
        # A dark-frame verdict is only attributable if this roll HAD a movie
        # frame to draw. Movie decode is roll-dependent (open no-movie class,
        # iter-440q): with snaps=0 there were no plane Locks at all, so even a
        # perfect renderer shows black and the roll proves NOTHING about it.
        # Surfaced here, in the table the verdict is actually read from, rather
        # than in a log line that is easy to skip at exactly the wrong moment.
        snaps      = 0     # [S4_STAGE] producer-side plane snapshots
        mQuads     = 0     # draws matching the movie quad shape (prim=13 count=4)
        arenaRej   = 0     # arena rejects; >0 = pixel data DROPPED, darkness is ours
        # ---- iter-463 phase 2 RUN CLASS (docs/DEVTOOLS_DESIGN.md 3.1/3.2) ----
        # "natural" only when the exe logged no [RUN_CLASS] tag, the last [GAUGE] carries
        # devtools=0x0 devtoolActs=0, and the roll started with no seeded saves. Anything else
        # (dev-tool-assisted, natural+hud, save-seeded, devtool-derived-save) is never a
        # natural-baseline result: tools/verify_run.py refuses it.
        runClass    = "natural"
        devtools    = "0x0"   # [GAUGE] devtools=<mask> (last line)
        devtoolActs = 0       # [GAUGE] devtoolActs=<n> (last line; uncapped atomic)
        crash      = $false
    }
    $r.crash = $r.logHealth.verdict -eq 'FAIL'
    if (-not $r.exists) { return $r }
    $content = Get-Content $logPath -Raw -ErrorAction SilentlyContinue
    if (-not $content) { return $r }
    $r.lines = ($content -split "`n").Count

    $pm = [regex]::Matches($content, "\[D3D_DIAG\] present #(\d+)")
    if ($pm.Count) { $r.present = ($pm | ForEach-Object { [int]$_.Groups[1].Value } | Measure-Object -Maximum).Maximum }

    $r.parser    = ([regex]::Matches($content, "sub_826AE6D0\(PARSER\)")).Count
    # iter-449q: [APT_LOADMOVIE] = the FE root movie load (sub_826BB6F0 path,
    # "main"/_level0) — fires exactly once per title-dispatched roll. The old
    # [LOADMOVIE] tag (AS builtin sub_826CB500) has NEVER fired naturally in
    # any archived roll and only counted retired forcing drivers.
    $r.loadMovie = ([regex]::Matches($content, "\[APT_LOADMOVIE\]")).Count

    # iter-358: the LAST [GAUGE] census line is the authoritative count. It is
    # emitted every 10s by a wall-clock thread, so the final one reflects the
    # whole run. If it is absent the run never bumped a natural gauge at all —
    # which is itself the finding, and leaves these columns honestly at 0.
    $gm = [regex]::Matches($content,
      "\[GAUGE\] \S+ \| feTick=(\d+) aptPump=(\d+) clipCtorAll=(\d+) clipCtorReal=(\d+) defExec=(\d+) aptReq=(\d+)")
    if ($gm.Count) {
        $g = $gm[$gm.Count - 1]
        $r.feFrames = [int]$g.Groups[1].Value
        $r.aptPump  = [int]$g.Groups[2].Value
        $r.clipCtor = [int]$g.Groups[3].Value
        $r.movInst  = [int]$g.Groups[4].Value
        $r.defExec  = [int]$g.Groups[5].Value
        $r.aptReq   = [int]$g.Groups[6].Value
    }
    # iter-463 phase 2: the dev-tool tag rides every [GAUGE] line; the [RUN_CLASS] lines are
    # logged once at boot (and at a marked LOAD / a sidecar write).
    $dm = [regex]::Matches($content, "\[GAUGE\] \S+ \|.*?devtools=(0x[0-9A-Fa-f]+) devtoolActs=(\d+)")
    if ($dm.Count) {
        $r.devtools    = $dm[$dm.Count - 1].Groups[1].Value
        $r.devtoolActs = [long]$dm[$dm.Count - 1].Groups[2].Value
    }
    $tags = @()
    foreach ($m in [regex]::Matches($content, "\[RUN_CLASS\] (\S+)")) {
        $t = $m.Groups[1].Value
        if ($t -eq "present-dump") { continue }
        if ($tags -notcontains $t) { $tags += $t }
    }
    if ($seedContentDir -and ($tags -notcontains "save-seeded")) { $tags += "save-seeded" }
    if ($r.devtoolActs -gt 0 -and ($tags -notcontains "dev-tool-assisted")) { $tags += "dev-tool-assisted" }
    if ($tags.Count) { $r.runClass = ($tags -join "+") }
    # iter-442 S4.2: last [S4_STAGE] line is authoritative (same cadence logic
    # as [GAUGE] above — counters are cumulative, the probe caps its lines).
    # iter-444 S6e: the line gained a `blocks=` group for the small-block pool.
    # The old pattern pinned the field ORDER, so inserting a group made it stop
    # matching entirely and the table reported NO-MOVIE for a roll whose log
    # said MOVIE-ACTIVE with 200,902 snapshots — a parser that fails CLOSED and
    # reports the healthy case as the dead one. Named lookups instead, so the
    # next field added between them cannot silently blank the gate.
    # iter-449c: the SAME failure fired again when the line gained a `geo=`
    # section between blocks and movieQuads — the named lookups survived, but
    # the pattern still pinned those two sections ADJACENT. The `.*?` bridge
    # tolerates any future section inserted there; only the named fields bind.
    $sm = [regex]::Matches($content,
      "\[S4_STAGE\] snaps=(?<snaps>\d+) .*?\brejects=(?<arej>\d+) \| " +
      "blocks=\d+ inflight=\d+ peak=\d+ rejects=(?<brej>\d+) \| " +
      ".*?movieQuads=(?<mq>\d+)")
    if ($sm.Count) {
        $s = $sm[$sm.Count - 1]
        $r.snaps    = [int]$s.Groups["snaps"].Value
        # Either pool lapping means data was dropped — surface the total.
        $r.arenaRej = [int]$s.Groups["arej"].Value + [int]$s.Groups["brej"].Value
        $r.mQuads   = [int]$s.Groups["mq"].Value
    }
    # APTCTX context established = an [APTCTX] line reporting 0x829A17BC=<nonzero>
    $ctxm = [regex]::Matches($content, "0x829A17BC=([0-9A-Fa-f]{8})")
    foreach ($m in $ctxm) { if ($m.Groups[1].Value -ne "00000000") { $r.aptCtxOk = 1; break } }
    if ($content -match "\[BUGCHECK\]|Guest harness: exception|\[CRASH\]") { $r.crash = $true }

    # ---- natural-boot signals ----
    # iter-326 recalibration: the EXTHREAD markers were cap-limited and read 0
    # while walks demonstrably completed. POST_8271E928 = uncapped
    # logInit2Progress marker, one per completed runner walk (4 = full init).
    $r.walksRet = ([regex]::Matches($content, "POST_8271E928")).Count
    $wd = [regex]::Matches($content, "E630_enter: val1=0x0000000(\d)")
    if ($wd.Count) { $r.walkDepth = ($wd | ForEach-Object { [int]$_.Groups[1].Value } | Measure-Object -Maximum).Maximum }
    $r.schedWork = ([regex]::Matches($content, "worked=[1-9]")).Count
    $fc = [regex]::Matches($content, "\[FC108\] call#(\d+)")
    if ($fc.Count) { $r.spinStorm = ($fc | ForEach-Object { [long]$_.Groups[1].Value } | Measure-Object -Maximum).Maximum }
    $li = [regex]::Matches($content, "F2C8_vt7_pre: val1=(0x[0-9A-Fa-f]{8})")
    if ($li.Count) { $r.lastInit = $li[$li.Count - 1].Groups[1].Value }
    if ($content -match "FSM released") { $r.init2Done = 1 }
    $r.frameDisp = ([regex]::Matches($content, "\[FRAME_DISPATCH\]")).Count
    $r.frameDumps = ([regex]::Matches($content, "\[FRAME_DUMP\] shot \d+ written")).Count
    return [pscustomobject]$r   # PSCustomObject so Measure-Object -Property / Sort-Object see the fields
}

$results = @()
foreach ($r in $procs) {
    $measured = Measure-Run $r.Log
    $r.Proc.Refresh()
    $exited = $r.Proc.HasExited
    $exitCode = $null
    if ($exited) {
        $r.Proc.WaitForExit()
        $exitCode = $r.Proc.ExitCode
    }
    # The wrapper's own exit is a reporting result, not the game's process exit.
    $measured | Add-Member -NotePropertyName processId -NotePropertyValue $r.Proc.Id
    $measured | Add-Member -NotePropertyName exited -NotePropertyValue $exited
    $measured | Add-Member -NotePropertyName exitCode -NotePropertyValue $exitCode
    $results += $measured
}

# Human-readable table. iter-358: every column here now measures a NATURAL
# signal off the uncapped [GAUGE] census. movInst = clip ctors with a REAL
# movieDef (the menu's north star: a movie actually instanced).
"`n=== run_recomp_x aggregate ($K runs, ${TimeoutSeconds}s) ==="
"{0,-4} {1,8} {2,8} {3,7} {4,7} {5,7} {6,8} {7,9} {8,8} {9,8} {10,9} {11,6}" -f "run","present","defExec","parser","aptCtx","loadMov","aptReq","clipCtor","movInst","aptPump","feFrames","crash"
for ($i = 0; $i -lt $results.Count; $i++) {
    $x = $results[$i]
    "{0,-4} {1,8} {2,8} {3,7} {4,7} {5,7} {6,8} {7,9} {8,8} {9,8} {10,9} {11,6}" -f $i,$x.present,$x.defExec,$x.parser,$x.aptCtxOk,$x.loadMovie,$x.aptReq,$x.clipCtor,$x.movInst,$x.aptPump,$x.feFrames,([int]$x.crash)
}

# Natural-boot table (iter-305: the drive-chain columns above measure the
# RETIRED band-aid chain; THESE are the live gauges).
"`n--- natural boot ---"
"{0,-4} {1,9} {2,10} {3,10} {4,10} {5,6} {6,10} {7,6} {8,12}" -f "run","walksRet","walkDepth","schedWork","spinStorm","init2","frameDisp","dumps","lastInitObj"
for ($i = 0; $i -lt $results.Count; $i++) {
    $x = $results[$i]
    "{0,-4} {1,9} {2,10} {3,10} {4,10} {5,6} {6,10} {7,6} {8,12}" -f $i,$x.walksRet,$x.walkDepth,$x.schedWork,$x.spinStorm,$x.init2Done,$x.frameDisp,$x.frameDumps,$x.lastInit
}

# iter-442 S4.2: THE MOVIE ACTIVITY GATE. Read this BEFORE judging any dark
# frame — GATE=NO-MOVIE means the roll had nothing to render and says nothing
# about the renderer either way.
"`n--- movie activity gate (S4) ---"
"{0,-4} {1,8} {2,9} {3,9} {4,24}" -f "run","snaps","mQuads","rejects","gate"
for ($i = 0; $i -lt $results.Count; $i++) {
    $x = $results[$i]
    $gate = if ($x.snaps -eq 0) { "NO-MOVIE (dark = n/a)" }
            elseif ($x.arenaRej -gt 0) { "ARENA-LAPPED (ours)" }
            else { "MOVIE-ACTIVE" }
    "{0,-4} {1,8} {2,9} {3,9} {4,24}" -f $i,$x.snaps,$x.mQuads,$x.arenaRej,$gate
}

# iter-463 phase 2: the run class (docs/DEVTOOLS_DESIGN.md 3.2). Only "natural" rows may back a
# NATURAL_BASELINE / milestone claim.
"`n--- run class (dev tools / saves) ---"
"{0,-4} {1,-40} {2,10} {3,12}" -f "run","runClass","devtools","devtoolActs"
for ($i = 0; $i -lt $results.Count; $i++) {
    $x = $results[$i]
    "{0,-4} {1,-40} {2,10} {3,12}" -f $i,$x.runClass,$x.devtools,$x.devtoolActs
}

$best = ($results | Sort-Object -Property @{e={$_.walksRet}}, @{e={$_.feFrames}}, @{e={$_.defExec}}, @{e={$_.movInst}}, @{e={$_.parser}}, @{e={$_.present}} -Descending | Select-Object -First 1)
$anyDef    = (($results | Measure-Object -Property defExec -Sum).Sum) -gt 0
$anyParser = (($results | Measure-Object -Property parser  -Sum).Sum) -gt 0
$anyCtx    = (($results | Measure-Object -Property aptCtxOk -Sum).Sum) -gt 0
$anyInst   = (($results | Measure-Object -Property movInst  -Sum).Sum) -gt 0
$maxFrames = (($results | Measure-Object -Property feFrames -Maximum).Maximum)
$maxPump   = (($results | Measure-Object -Property aptPump  -Maximum).Maximum)
$maxWalks  = (($results | Measure-Object -Property walksRet -Maximum).Maximum)
$fullInit  = @($results | Where-Object { $_.walksRet -ge 4 }).Count
# iter-358: "REAL MOVIE INSTANCED" = a clip ctor with a non-NULL movieDef, the
# natural signal that the menu path produced an actual movie (not the retired
# [BLOB_INJECT] forced install this column used to count).
"`nVERDICT: FULL-INIT rolls = {0}/{1} (max walksRet {2}) | DEF_EXEC = {3} | PARSER = {4} | aptCtx = {5} | REAL MOVIE INSTANCED = {6} | max natural FE ticks = {7} | max Apt pump ticks = {8}" -f $fullInit,$K,$maxWalks,$anyDef,$anyParser,$anyCtx,$anyInst,$maxFrames,$maxPump

# Runtime health is independent of progress and the process's exit status.
# Keep expected native refusal markers available to negative-control graders.
$healthExit = Get-SrRunHealthExit $results
"RUN HEALTH: {0}" -f $(if ($healthExit -eq 0) { 'HEALTHY (not gameplay acceptance)' } else { 'FAIL OR UNKNOWN; diagnostic artifacts retained' })

# Machine-readable summary for the analysis workflow / loop.
$summary = [ordered]@{
    k             = $K
    timeoutSec    = $TimeoutSeconds
    gates         = $gateMap
    contentSeed   = $(if ($seedContentDir) { $seedContentDir } else { "" })   # iter-463: "" = every run started with NO saves
    contentDirs   = @($procs | ForEach-Object { $_.ContentDir })
    allNatural    = (@($results | Where-Object { $_.runClass -ne "natural" }).Count -eq 0)   # iter-463 phase 2
    allLogHealthy = ($healthExit -eq 0)
    runs          = $results
    anyDefExec    = $anyDef
    anyParser     = $anyParser
    anyAptCtx     = $anyCtx
    anyMovieInst  = $anyInst
    maxFeFrames   = $maxFrames
    maxWalksRet   = $maxWalks
    fullInitRuns  = $fullInit
    bestRunIndex  = $results.IndexOf($best)
}
$outPath = Join-Path $PSScriptRoot "last_run_x.json"
$summary | ConvertTo-Json -Depth 6 | Out-File -FilePath $outPath -Encoding utf8
"Summary written: $outPath"
exit $healthExit
