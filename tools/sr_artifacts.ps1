# sr_artifacts.ps1 - artifact-root preflight and attempt allocation.
#
# WHY THIS IS A SEPARATE FILE (director ruling, iter-450X): the previous version
# of this logic lived inside sr_drive.ps1, so the only way to test it was to
# invoke the driver - which launches and kills the real game. A suite labelled
# "zero rolls" was doing neither of those things honestly. Allocation and
# preflight now live in production code that tests dot-source and call directly,
# with no process action of any kind.
#
# WHAT IT GUARANTEES
#   - A non-empty -Out is REFUSED. There is no -Fresh and no -Reuse: iter-450X
#     shipped both and both were defects. -Reuse reset numbering to attempt1 and
#     overwrote that attempt's provenance, outcome, failure file and frames -
#     the exact collision the fix claimed to prevent. -Fresh recursively deleted
#     a caller-supplied path with no safe-root constraint, which is a foot-gun
#     pointed at the whole filesystem. Fail closed; make the operator pick a new
#     directory. It costs them one word and cannot corrupt an evidence bank.
#   - Attempt roots are monotonic and immutable within an invocation. Allocation
#     REFUSES an attempt directory that already exists rather than writing into
#     it.
#   - Every outcome artifact - steps.log, provenance, outcome, failure reason,
#     frames, run log - lives inside its owning attempt.
#
# ASCII only: PowerShell 5.1 reads a BOM-less .ps1 as cp1252.

# ⛔ NO Set-StrictMode HERE. This file is DOT-SOURCED by sr_drive.ps1, and
# Set-StrictMode applies to the CALLER's scope - so declaring it made the driver
# throw "The property 'expect' cannot be found on this object" the moment a
# scenario step omitted an optional field. Every scenario has optional fields, so
# no acquisition could run. A library must not change the language semantics of
# the script that loads it. Found by the harness's second live acquisition
# attempt, iter-454.

# Preflight. Returns a result object; NEVER throws, never deletes, never touches
# a process. The caller decides what to do with a refusal, which is what lets a
# test assert the refusal without a subprocess.
function Test-SrArtifactRoot {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Out)

    $result = [ordered]@{
        Out        = $Out
        Exists     = $false
        FileCount  = 0
        DirCount   = 0
        EntryCount = 0
        Ok         = $true
        Reason     = $null
    }
    # The whole probe is wrapped: a preflight whose contract is "fail closed"
    # must never THROW. An illegal path made Test-Path raise an argument
    # exception, which propagated past every refusal path (found by the E7
    # adversarial test, iter-450Z).
    try {
    if (Test-Path -LiteralPath $Out) {
        $result.Exists = $true
        # ⛔ -Force IS LOAD-BEARING. Without it Get-ChildItem SKIPS hidden and
        # system entries, so a stale bank containing a hidden frame passed
        # preflight and was silently mixed in (director finding, iter-450Y;
        # measured: 0 seen without -Force, 1 with).
        # ⛔ And directories count. Counting only files accepted an -Out that
        # already held attempt1/frames/ with no files yet - preflight said OK,
        # then allocation THREW, turning a controlled refusal into an
        # unhandled error.
        # ⛔ ENUMERATION ERRORS FAIL CLOSED. -ErrorAction SilentlyContinue alone
        # meant an unreadable subtree counted as zero entries, so a root we
        # could not inspect was treated as safe (director finding, iter-450Y).
        $enumErrors = @()
        $entries = @(Get-ChildItem -LiteralPath $Out -Recurse -Force `
                       -ErrorAction SilentlyContinue -ErrorVariable +enumErrors)
        if ($enumErrors.Count -gt 0) {
            $result.Ok = $false
            $result.Reason = ("sr_drive: -Out '{0}' could not be fully enumerated ({1} error(s), first: {2}). " +
                              "Refusing: an artifact root we cannot inspect cannot be certified empty." `
                              -f $Out, $enumErrors.Count, $enumErrors[0].Exception.Message)
            $result.EntryCount = $entries.Count
            return [pscustomobject]$result
        }
        $result.FileCount  = @($entries | Where-Object { -not $_.PSIsContainer }).Count
        $result.DirCount   = @($entries | Where-Object { $_.PSIsContainer }).Count
        $result.EntryCount = $entries.Count
        if ($entries.Count -gt 0) {
            $result.Ok = $false
            $result.Reason = ("sr_drive: -Out '{0}' already contains {1} entr(ies) ({2} file(s), " +
                              "{3} director(ies), hidden included). Refusing: mixing two invocations " +
                              "into one artifact root silently corrupted an evidence bank (iter-450W). " +
                              "Choose a new -Out." -f $Out, $entries.Count, $result.FileCount, $result.DirCount)
        }
    }
    } catch {
        $result.Ok = $false
        $result.Reason = ("sr_drive: -Out '{0}' could not be probed ({1}). Refusing." -f $Out, $_.Exception.Message)
    }
    return [pscustomobject]$result
}

# Atomically CLAIM the artifact root, closing the check-then-create race between
# Test-SrArtifactRoot and the first write (director finding, iter-450Y): two
# concurrent drivers could both pass preflight and then both allocate attempt1.
# The claim marker is created with CreateNew, which fails if it already exists,
# so exactly one caller wins. The root is re-verified AFTER the claim so
# anything that appeared during the window is still caught.
function Lock-SrArtifactRoot {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Out)

    New-Item -ItemType Directory -Force $Out | Out-Null
    # ⛔ RESOLVE TO AN ABSOLUTE PATH FIRST. The marker was built from the caller's
    # -Out and then compared against Get-ChildItem's FullName, which is always
    # absolute. With a RELATIVE -Out (the normal production form,
    # `-Out archive\iter454_flat`) the two never match, the marker counts as
    # foreign content, and EVERY first-ever claim refuses with "gained 1
    # entr(ies) between preflight and claim". The test suite missed it because
    # it always passed an absolute $tmp path. Found by the harness's first live
    # acquisition, iter-454 - which is precisely the freeze-exemption condition.
    $Out = (Resolve-Path -LiteralPath $Out).Path
    $marker = Join-Path $Out '.sr_claim'
    try {
        $fs = [System.IO.File]::Open($marker, [System.IO.FileMode]::CreateNew,
                                     [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
        try {
            $bytes = [Text.Encoding]::ASCII.GetBytes(
                ("claimed {0} by pid {1}`n" -f (Get-Date).ToUniversalTime().ToString('o'), $PID))
            $fs.Write($bytes, 0, $bytes.Length)
        } finally { $fs.Dispose() }
    } catch {
        return [pscustomobject]@{ Ok = $false; Reason =
            ("sr_drive: -Out '{0}' is already claimed by another invocation (.sr_claim exists). " +
             "Refusing to share an artifact root." -f $Out) }
    }
    # Re-verify: only the claim marker may exist. ⛔ This recheck ALSO fails
    # closed on enumeration errors - the first version silenced them, repeating
    # the very defect just fixed in Test-SrArtifactRoot fifty lines above
    # (director finding, iter-450Z).
    $reErrors = @()
    $entries = @(Get-ChildItem -LiteralPath $Out -Recurse -Force `
                   -ErrorAction SilentlyContinue -ErrorVariable +reErrors |
                 Where-Object { $_.FullName -ne $marker })
    if ($reErrors.Count -gt 0) {
        return [pscustomobject]@{ Ok = $false; Reason =
            ("sr_drive: -Out '{0}' could not be re-enumerated after claiming ({1} error(s)). Refusing." -f $Out, $reErrors.Count) }
    }
    if ($entries.Count -gt 0) {
        return [pscustomobject]@{ Ok = $false; Reason =
            ("sr_drive: -Out '{0}' gained {1} entr(ies) between preflight and claim. Refusing." -f $Out, $entries.Count) }
    }
    return [pscustomobject]@{ Ok = $true; Reason = $null; Marker = $marker }
}

# FRAME SCRATCH (owner, 2026-10-11). Raw present dumps go to an SSD scratch root
# (default J:\sr_dumps, used only when that folder exists): BMP writes on the D:
# HDD stalled presents and voided rolls. <attempt>\frames is a directory JUNCTION
# to a fresh scratch folder, so every reader keeps the same path.
# SR_FRAME_SCRATCH = another root, or 0 / off to keep frames inside the attempt.
# Raw frames there are scratch (owner: deleted automatically once graded): the GC
# below deletes a scratch folder 48 h after it was made - sooner, oldest first,
# when the root holds more than 200 GB and the folder is at least 6 h old -
# removes only its junction (never the attempt) and leaves frames_deleted.txt.
# Graded outputs and copied evidence live on D: and are never touched. Copy any
# frame you need as lasting evidence into the archive before the 48 h are up.
# MEASURE BEFORE DELETING (2026-10-14). The world rule (tools/perf/world_floor.py,
# shared by the flicker gate and tools/native_ledger.py) grades a roll from its
# frames: deleting them left 67 archived attempts grading "world 0 / unmeasured" -
# missing input, not a rendering failure. Before the GC deletes a folder that an
# attempt's junction points at, world_floor.py measures EVERY BMP in it into
# <attempt>\frames_measured.json (the measurements, never a verdict); the world
# rule reads that file when a BMP is absent. If measuring fails the folder is KEPT
# (a warning; explicit GC retries). Launch-time GC only reuses an existing,
# validated sidecar: measuring old BMPs must not consume a new run's timeout.
# Invoke-SrFrameScratchGc without -ExistingMeasurementsOnly still measures first.
# SR_PYTHON overrides the interpreter.
$script:SrFrameScratchKeepHours = 48
$script:SrFrameScratchQuotaGB   = 200
$script:SrFrameScratchMinAgeH   = 6
$script:SrFrameScratchLeafRx    = '^\d{8}-\d{6}_[0-9a-f]{8}_a\d+$'
$script:SrWorldFloorPy          = Join-Path $PSScriptRoot 'perf\world_floor.py'

function Get-SrFrameScratchRoot {
    $e = $env:SR_FRAME_SCRATCH
    if ($e -and ($e -eq '0' -or $e -ieq 'off')) { return $null }
    $root = if ($e) { $e } else { 'J:\sr_dumps' }
    if (Test-Path -LiteralPath $root -PathType Container) { return (Resolve-Path -LiteralPath $root).Path.TrimEnd('\') }
    return $null
}

# Removes a junction's LINK only. Never Remove-Item -Recurse on a junction: PS 5.1
# follows it and deletes the target's contents. RemoveDirectory on a reparse point
# deletes the link itself, whatever the target holds.
function Remove-SrJunction([string]$Path) {
    $it = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if (-not $it -or -not ($it.Attributes -band [IO.FileAttributes]::ReparsePoint)) { return $false }
    [System.IO.Directory]::Delete($Path, $false)
    return $true
}

# Measures every BMP of an attempt's frames folder into <attempt>\frames_measured.json
# with tools/perf/world_floor.py (center and sides, straight from the pixels).
# Never throws. Ok only when ALL of these hold: the GC's own enumeration of the
# folder succeeds (an unreadable folder is never an empty one); the measurer exits 0;
# it prints SIDECAR_OK <n> (and the file exists) or SIDECAR_EMPTY 0; and <n> equals
# the GC's own count of *.bmp there. Same stderr discipline as Invoke-SrGit: a native
# command's stderr must not become a terminating error.
function Invoke-SrWorldFloorPy {
    param([Parameter(Mandatory = $true)][string[]]$PyArgs)
    $py = if ($env:SR_PYTHON) { $env:SR_PYTHON } else { 'python' }
    if (-not (Test-Path -LiteralPath $script:SrWorldFloorPy -PathType Leaf)) {
        return [pscustomobject]@{ Code = -1; Out = "world_floor.py not found at '$($script:SrWorldFloorPy)'" }
    }
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $global:LASTEXITCODE = 0
        $out = (& $py $script:SrWorldFloorPy @PyArgs 2>&1 | Out-String)
        return [pscustomobject]@{ Code = $LASTEXITCODE; Out = $out }
    } catch {
        return [pscustomobject]@{ Code = -1; Out = $_.Exception.Message }
    } finally {
        $ErrorActionPreference = $prev
    }
}

# Ok only when world_floor.py confirms that the attempt's frames_measured.json APPLIES
# (schema, version, run.log binding, valid values, its fixed inventory reconciled) and
# holds every file in $Scratch as THAT version (size, and each BMP's mtime): the GC's
# last check before it deletes. -Exact: $Scratch holds exactly the inventory's files and
# their bytes equal its total (returned as Total, what frames_deleted.txt must record).
# Never throws.
function Test-SrFrameMeasurementsCover {
    param([Parameter(Mandatory = $true)][string]$FrameDir, [Parameter(Mandatory = $true)][string]$Scratch,
          [switch]$Exact)
    $pyArgs = @('covers', $FrameDir, $Scratch)
    if ($Exact) { $pyArgs += '--exact' }
    $r = Invoke-SrWorldFloorPy -PyArgs $pyArgs
    $total = $null
    $ok = $false
    $want = if ($Exact) { 'exact' } else { 'subset' }
    if (($r.Code -eq 0) -and ("" + $r.Out) -match ('COVERS_OK (\d+) (\d+) ' + $want + ' ')) {
        $ok = $true
        $total = [long]$Matches[2]
    }
    return [pscustomobject]@{ Ok = $ok; Code = $r.Code; Total = $total; Detail = ("" + $r.Out).Trim() }
}

function Save-SrFrameMeasurements {
    param([Parameter(Mandatory = $true)][string]$FrameDir)
    $sidecar = Join-Path (Split-Path -Parent $FrameDir) 'frames_measured.json'
    try {
        $own = @(Get-ChildItem -LiteralPath $FrameDir -File -Force -ErrorAction Stop |
                 Where-Object { $_.Name -like '*.bmp' }).Count
    } catch {
        return [pscustomobject]@{ Ok = $false; Sidecar = $sidecar; Code = -1
                                  Detail = ("could not enumerate the frames: " + $_.Exception.Message) }
    }
    $r = Invoke-SrWorldFloorPy -PyArgs @('sidecar', $FrameDir)
    $code = $r.Code
    $out = "" + $r.Out
    $n = -1
    $kind = ''
    if ($out -match 'SIDECAR_(OK|EMPTY) (\d+) ') { $kind = $Matches[1]; $n = [int]$Matches[2] }
    $ok = ($code -eq 0) -and ($n -eq $own) -and
          ((($kind -eq 'EMPTY') -and ($n -eq 0)) -or
           (($kind -eq 'OK') -and ($n -gt 0) -and (Test-Path -LiteralPath $sidecar -PathType Leaf)))
    $detail = $out.Trim()
    if ($code -eq 0 -and $n -ne $own) {
        $detail = ("the measurer reported {0} BMP(s), the folder holds {1}: {2}" -f $n, $own, $detail)
    }
    $total = $null
    if ($ok -and $kind -eq 'OK') {
        # read it back the way the world rule will: it must apply, and its fixed inventory must be exactly this
        # folder (every file, every version, the byte total frames_deleted.txt will record)
        $c = Test-SrFrameMeasurementsCover -FrameDir $FrameDir -Scratch $FrameDir -Exact
        if (-not $c.Ok) { $ok = $false; $code = $c.Code; $detail = "written, but it is not this folder's inventory: " + $c.Detail }
        else { $total = $c.Total }
    }
    return [pscustomobject]@{ Ok = $ok; Sidecar = $sidecar; Code = $code; Detail = $detail; Total = $total }
}

# Deletes expired scratch folders under $Root (only names this file creates).
# Never throws: a GC problem must not stop a roll. Returns the deleted folders.
# FAIL CLOSED (Astra 2026-10-14): every step here is terminating ($ErrorActionPreference
# 'Stop', scoped to this function) and ANY error, or anything the GC cannot establish,
# KEEPS the folder it was about (a warning names it; the next roll's GC retries).
# A folder is deleted only when its back-reference (<leaf>.source.txt) reads cleanly
# and names an attempt frames path that exists, and then only if
#   - that path is a junction to THIS folder and Save-SrFrameMeasurements succeeds
#     (every BMP measured into the attempt's frames_measured.json), or
#   - it is a junction to ANOTHER folder (the attempt was re-allocated: these frames are
#     superseded and no reader reaches them), or
#   - it is a plain folder beside a frames_measured.json that APPLIES and measures every
#     BMP here (world_floor.py covers: a previous pass measured, replaced the junction,
#     then failed before deleting).
# KEPT: no or empty back-reference, an attempt path that is gone (deleted or MOVED - a
# moved attempt still points here), a plain folder with no measurements, any error.
function Invoke-SrFrameScratchGc {
    param([Parameter(Mandatory = $true)][string]$Root,
          [double]$KeepHours = $script:SrFrameScratchKeepHours,
          [double]$QuotaGB = $script:SrFrameScratchQuotaGB,
          [double]$MinAgeHours = $script:SrFrameScratchMinAgeH,
          [datetime]$Now = (Get-Date),
          [switch]$ExistingMeasurementsOnly)
    $ErrorActionPreference = 'Stop'
    $removed = New-Object System.Collections.Generic.List[string]
    try {
        $rootFull = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
        $dirs = @(Get-ChildItem -LiteralPath $rootFull -Directory -Force |
                  Where-Object { -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -and
                                 $_.Name -match $script:SrFrameScratchLeafRx } |
                  Sort-Object CreationTime)
    } catch {
        Write-Warning ("frame scratch GC: nothing deleted, cannot list {0}: {1}" -f $Root, $_.Exception.Message)
        return ,$removed.ToArray()
    }
    $sizes = @{}
    $unreadable = @{}
    $total = 0.0
    foreach ($d in $dirs) {
        try {
            $s = (Get-ChildItem -LiteralPath $d.FullName -File -Force | Measure-Object -Property Length -Sum).Sum
            $sizes[$d.FullName] = [double]$(if ($s) { $s } else { 0 })
        } catch {
            $sizes[$d.FullName] = 0.0
            $unreadable[$d.FullName] = $_.Exception.Message
        }
        $total += $sizes[$d.FullName]
    }
    foreach ($d in $dirs) {
        try {
            $age = ($Now - $d.CreationTime).TotalHours
            $over = $total -gt ($QuotaGB * 1GB)
            if (-not ($age -ge $KeepHours -or ($over -and $age -ge $MinAgeHours))) { continue }
            $why = $null
            if ($unreadable.ContainsKey($d.FullName)) { $why = "its files cannot be listed: " + $unreadable[$d.FullName] }
            $src = Join-Path $rootFull ($d.Name + '.source.txt')
            $link = ''
            if (-not $why) {
                if (-not (Test-Path -LiteralPath $src -PathType Leaf)) {
                    $why = "no back-reference ($src): which attempt owns these frames is unknown"
                } else {
                    $link = ("" + (Get-Content -LiteralPath $src -TotalCount 1)).Trim()
                    if (-not $link) { $why = "its back-reference ($src) is empty" }
                }
            }
            if (-not $why -and -not (Test-Path -LiteralPath $link)) {
                $why = "its attempt frames path $link is gone (the attempt was deleted, or MOVED and still points here): check, then delete by hand"
            }
            $measured = $null
            if (-not $why) {
                $li = Get-Item -LiteralPath $link -Force
                $isJunction = [bool]($li.Attributes -band [IO.FileAttributes]::ReparsePoint)
                $tgt = if ($isJunction) { @($li.Target) | Select-Object -First 1 } else { $null }
                $sidecar = Join-Path (Split-Path -Parent $link) 'frames_measured.json'
                if ($isJunction -and $tgt -and ($tgt.TrimEnd('\') -ieq $d.FullName.TrimEnd('\'))) {
                    if ($ExistingMeasurementsOnly) {
                        if (-not (Test-Path -LiteralPath $sidecar -PathType Leaf)) {
                            $why = 'measurements deferred until explicit GC; no existing frames_measured.json'
                            $m = $null
                        } else {
                            $c = Test-SrFrameMeasurementsCover -FrameDir $link -Scratch $d.FullName -Exact
                            $m = [pscustomobject]@{ Ok = $c.Ok; Sidecar = $sidecar; Code = $c.Code
                                                  Total = $c.Total; Detail = $c.Detail }
                        }
                    } else {
                        $m = Save-SrFrameMeasurements -FrameDir $link
                    }
                    if (-not $why -and -not $m.Ok) {
                        $why = "measuring its frames into {0} failed (exit {1}): {2}" -f $m.Sidecar, $m.Code, $m.Detail
                    } elseif (-not $why) {
                        $measured = $m
                    }
                } elseif ($isJunction -and $tgt) {
                    # re-allocated: the attempt's frames are another scratch folder now - these are superseded
                } elseif ($isJunction) {
                    $why = "its attempt frames path $link is a junction whose target cannot be read"
                } elseif (-not (Test-Path -LiteralPath $sidecar -PathType Leaf)) {
                    $why = "its attempt frames path $link is a plain folder and no frames_measured.json says these frames were measured"
                } else {
                    $c = Test-SrFrameMeasurementsCover -FrameDir $link -Scratch $d.FullName
                    if (-not $c.Ok) {
                        $why = "its attempt frames path $link is a plain folder and its frames_measured.json does not cover these frames: " + $c.Detail
                    }
                }
            }
            if ($why) {
                Write-Warning ("frame scratch GC: KEPT {0}: {1}" -f $d.FullName, $why)
                continue
            }
            if ($measured) {
                # The deletion record must equal the sidecar's fixed inventory (world_floor's reader rejects it
                # otherwise): with a sidecar, record the total the exact cover check measured - and only if it is
                # what this GC sized the folder at, i.e. nothing changed in between.
                $bytes = [double]$sizes[$d.FullName]
                if ($null -ne $measured.Total) {
                    if ([double]$measured.Total -ne $bytes) {
                        throw ("the folder changed while it was measured ({0} bytes sized, {1} in the inventory)" -f $bytes, $measured.Total)
                    }
                    $bytes = [double]$measured.Total
                }
                if (-not (Remove-SrJunction $link)) { throw "could not remove the junction $link" }
                New-Item -ItemType Directory -Force $link | Out-Null
                $kept = if (Test-Path -LiteralPath $measured.Sidecar -PathType Leaf) { '; measurements kept in frames_measured.json' } else { '' }
                $inv = [Globalization.CultureInfo]::InvariantCulture
                Set-Content -LiteralPath (Join-Path (Split-Path -Parent $link) 'frames_deleted.txt') -Encoding ascii `
                    -Value ("raw frames deleted {0} by the frame scratch GC (were {1}, {2} bytes){3}" -f
                            $Now.ToString('yyyy-MM-dd HH:mm', $inv), $d.FullName, ([long]$bytes).ToString('N0', $inv), $kept)
            }
            [System.IO.Directory]::Delete($d.FullName, $true)
            Remove-Item -LiteralPath $src -Force
            $total -= $sizes[$d.FullName]
            $removed.Add($d.FullName)
        } catch {
            Write-Warning ("frame scratch GC: KEPT {0}: {1}" -f $d.FullName, $_.Exception.Message)
        }
    }
    return ,$removed.ToArray()
}

# Allocate attempt N. Refuses a directory that already exists - monotonic and
# immutable, so a retry can never land on a previous attempt's frames.
function New-SrAttemptRoot {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Out,
        [Parameter(Mandatory = $true)][int]$Attempt
    )
    $dir = Join-Path $Out ("attempt{0}" -f $Attempt)
    if (Test-Path -LiteralPath $dir) {
        throw ("sr_drive: attempt root '{0}' already exists; refusing to overwrite it" -f $dir)
    }
    New-Item -ItemType Directory -Force $dir | Out-Null
    $frames = Join-Path $dir 'frames'
    $scratch = $null
    $scratchRoot = Get-SrFrameScratchRoot
    if ($scratchRoot) {
        $null = Invoke-SrFrameScratchGc -Root $scratchRoot -ExistingMeasurementsOnly
        $leaf = '{0}_{1}_a{2}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'),
                                  [guid]::NewGuid().ToString('N').Substring(0, 8), $Attempt
        $scratch = Join-Path $scratchRoot $leaf
        New-Item -ItemType Directory -Force $scratch | Out-Null
        $framesFull = Join-Path (Resolve-Path -LiteralPath $dir).Path 'frames'
        Set-Content -LiteralPath (Join-Path $scratchRoot ($leaf + '.source.txt')) -Value $framesFull -Encoding ascii
        New-Item -ItemType Junction -Path $framesFull -Target $scratch | Out-Null
    } else {
        New-Item -ItemType Directory -Force $frames | Out-Null
    }
    return [pscustomobject][ordered]@{
        AttemptDir    = $dir
        FrameDir      = $frames
        FrameScratch  = $scratch
        StepsLog      = Join-Path $dir 'steps.log'
        ProvenanceJson= Join-Path $dir 'provenance.json'
        OutcomeJson   = Join-Path $dir 'outcome.json'
        FailureTxt    = Join-Path $dir 'failure_reason.txt'
        RunLog        = Join-Path $dir 'run.log'
    }
}

# git writes advisory notes (CRLF conversion, detached HEAD, ...) to stderr even
# on success. Under $ErrorActionPreference='Stop' PowerShell 5.1 wraps those in
# a TERMINATING NativeCommandError, so a harmless warning aborts the caller.
# Isolate every git call behind this: stderr discarded, preference restored, and
# a failed git yields '' rather than killing the run.
# ⛔ Returns an OBJECT, not a string. Discarding stderr while ignoring the exit
# code meant a FAILED git produced an empty status, which then persisted as
# worktreeDirty=False - unavailable source identity looking clean (director
# finding, iter-450Z). Callers must check .Ok and fail closed.
function Invoke-SrGit {
    param([Parameter(Mandatory = $true)][string[]]$GitArgs)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $global:LASTEXITCODE = 0
        $out = (& git @GitArgs 2>$null | Out-String)
        return [pscustomobject]@{ Ok = ($LASTEXITCODE -eq 0); Code = $LASTEXITCODE; Text = $out.Trim() }
    } catch {
        return [pscustomobject]@{ Ok = $false; Code = -1; Text = '' }
    } finally {
        $ErrorActionPreference = $prev
    }
}

# ⛔ THIS MUST NEVER THROW. `Get-ChildItem Env:` raises "An item with the same
# key has already been added" when the process environment holds both PATH and
# Path - and in the driver this ran AFTER the existing-game Stop-Process sweep,
# so the first live invocation would have killed the operator's game and then
# died without launching a replacement (director finding, iter-450Z). Read the
# raw environment block defensively and degrade to a recorded failure instead.
function Get-SrEnvSnapshot {
    param([string]$Prefix = 'SR_')
    $snap = [ordered]@{}
    $err = $null
    try {
        $block = [System.Environment]::GetEnvironmentVariables()
        foreach ($k in @($block.Keys)) {
            $name = [string]$k
            if ($name -like "$Prefix*") { $snap[$name] = [string]$block[$k] }
        }
    } catch {
        $err = $_.Exception.Message
        # Last resort: ask for the variables we can name without enumerating.
        try {
            foreach ($n in @('SR_INSTANCE_ID', 'SR_PRESENT_DUMP_DIR', 'SR_KB_PAD_OBS', 'SR_CONTENT_DIR')) {
                $v = [System.Environment]::GetEnvironmentVariable($n)
                if ($null -ne $v) { $snap[$n] = $v }
            }
        } catch { }
    }
    return [pscustomobject]@{ Vars = $snap; Ok = ($null -eq $err); Error = $err }
}

function Get-SrFileHashSafe {
    param([string]$Path)
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return $null }
    try { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash } catch { return $null }
}

# Astra 088 (Option B): WHICH GAME EXECUTABLE the driver launches, resolved ONCE.
# The driver used to hardcode build\SupermanReturns.exe AND, separately, the
# process basename its pre-launch sweep uses - so a second executable would
# have been hashed/launched under one identity and swept under another. One
# resolved object now feeds hashing, provenance, the sweep and the launch.
#   - no -ExePath: EXACTLY the old default (path build\SupermanReturns.exe,
#     name SupermanReturns, sweep {SupermanReturns}); existence is still judged
#     by provenance, as before.
#   - -ExePath: resolved against the repo root when relative; must be an
#     existing FILE ending in .exe, else refused (no process is touched).
#     Its basename becomes the process name, and the sweep covers BOTH the
#     default and the selected basename, so a stale instance of either cannot
#     survive into (or be confused with) the new run.
# Pure: no process action, no file writes - the test suite calls it directly.
function Resolve-SrGameExe {
    param([Parameter(Mandatory=$true)][string]$Root, [string]$ExePath = '')
    $defaultName = 'SupermanReturns'
    $defaultPath = Join-Path $Root 'build\SupermanReturns.exe'
    if (-not $ExePath) {
        return [pscustomobject]@{ Ok = $true; Path = $defaultPath; Name = $defaultName
                                  IsDefault = $true; SweepNames = @($defaultName); Reason = '' }
    }
    $cand = if ([System.IO.Path]::IsPathRooted($ExePath)) { $ExePath } else { Join-Path $Root $ExePath }
    if (-not (Test-Path -LiteralPath $cand -PathType Leaf)) {
        return [pscustomobject]@{ Ok = $false; Path = $cand; Name = ''; IsDefault = $false
                                  SweepNames = @(); Reason = "-ExePath '$ExePath' is not an existing file" }
    }
    $full = (Resolve-Path -LiteralPath $cand).Path
    if ([System.IO.Path]::GetExtension($full) -ne '.exe') {
        return [pscustomobject]@{ Ok = $false; Path = $full; Name = ''; IsDefault = $false
                                  SweepNames = @(); Reason = "-ExePath '$ExePath' is not an .exe" }
    }
    $name = [System.IO.Path]::GetFileNameWithoutExtension($full)
    $sweep = @($defaultName)
    if ($name -ne $defaultName) { $sweep += $name }
    $isDefault = ($full -eq $defaultPath)
    return [pscustomobject]@{ Ok = $true; Path = $full; Name = $name; IsDefault = $isDefault
                              SweepNames = $sweep; Reason = '' }
}

# 2026-10-14 roll-stage (owner: "okay do them on j just be wary about space and resources"): sr_drive -Stage
# copies the game's READ set (the exe folder, the disc, the installed shader cache) to an SSD folder through
# tools/perf/stage_roll.py and launches the staged exe. The working directory stays the repo root, so every
# OUTPUT (log, frames, perf dir, saves, dumps) lands where it does without -Stage. -JsonPath receives the
# tool's record (sr_drive: <Out>\stage.json); -Note gets each output line as it comes. Never throws: a refusal
# (no space above the 50 GiB floor, SR_MODS / SR_DEVTOOLS, a foreign folder, a link, an incomplete disc) or an
# error is Ok=$false + Reason. Every staged file is re-hashed (--verify full). -LeaseId: the entries are leased to
# THIS process (pid + creation time) under the stage lock, so no clean or retention can remove them until
# Invoke-SrStageRelease, the driver's exit, or the lease's 12 h timeout.
$script:SrStageRollPy = Join-Path $PSScriptRoot 'perf\stage_roll.py'
function Invoke-SrStage {
    param([Parameter(Mandatory = $true)][string]$Stage, [Parameter(Mandatory = $true)][string]$ExePath,
          [Parameter(Mandatory = $true)][string]$Root, [Parameter(Mandatory = $true)][string]$JsonPath,
          [string]$Source = '', [string]$LeaseId = '', [scriptblock]$Note = $null)
    $fail = { param($why) [pscustomobject]@{ Ok = $false; Reason = $why; ExePath = ''; ShaderCacheDir = ''
                                             NbAotDir = ''; ContentRoot = ''; Record = $null } }
    $py = if ($env:SR_PYTHON) { $env:SR_PYTHON } else { 'python' }
    if (-not (Test-Path -LiteralPath $script:SrStageRollPy -PathType Leaf)) {
        return (& $fail "stage_roll.py not found at '$($script:SrStageRollPy)'")
    }
    $pyArgs = @('-u', $script:SrStageRollPy, 'stage', '--stage', $Stage, '--exe', $ExePath, '--root', $Root,
                '--json', $JsonPath, '--verify', 'full')
    if ($Source) { $pyArgs += @('--source', $Source) }
    if ($LeaseId) { $pyArgs += @('--lease-id', $LeaseId, '--lease-pid', [string]$PID, '--lease-note', "sr_drive $JsonPath") }
    $lines = New-Object System.Collections.ArrayList
    $code = -1
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if (Test-Path -LiteralPath $JsonPath) { Remove-Item -LiteralPath $JsonPath -Force }
        $global:LASTEXITCODE = 0
        & $py @pyArgs 2>&1 | ForEach-Object {
            $l = "$_"
            [void]$lines.Add($l)
            if ($Note) { & $Note $l }
        }
        $code = $LASTEXITCODE
    } catch {
        return (& $fail ("stage_roll.py could not run ({0}): {1}" -f $py, $_.Exception.Message))
    } finally {
        $ErrorActionPreference = $prev
    }
    if ($code -ne 0) {
        $last = @($lines | Where-Object { $_ -match '^(REFUSED|ERROR):' } | Select-Object -Last 1)
        $why = if ($last.Count) { $last[0] } elseif ($lines.Count) { $lines[$lines.Count - 1] } else { 'no output' }
        return (& $fail ("stage_roll.py exit {0}: {1}" -f $code, $why))
    }
    $rec = $null
    try { $rec = Get-Content -LiteralPath $JsonPath -Raw -ErrorAction Stop | ConvertFrom-Json } catch { }
    if (-not $rec -or -not $rec.exe) { return (& $fail "stage_roll.py exited 0 but left no record at $JsonPath") }
    $stageFull = [System.IO.Path]::GetFullPath($Stage).TrimEnd('\') + '\'
    $exeFull = [System.IO.Path]::GetFullPath([string]$rec.exe)
    if (-not $exeFull.StartsWith($stageFull, [StringComparison]::OrdinalIgnoreCase)) {
        return (& $fail "the staged exe $exeFull is not inside $Stage")
    }
    if (-not (Test-Path -LiteralPath $exeFull -PathType Leaf)) { return (& $fail "the staged exe $exeFull does not exist") }
    if ([System.IO.Path]::GetFileName($exeFull) -ne [System.IO.Path]::GetFileName($ExePath)) {
        return (& $fail "the staged exe $exeFull is not named like $ExePath (the sweep keys on the name)")
    }
    return [pscustomobject]@{ Ok = $true; Reason = ''; ExePath = $exeFull; ShaderCacheDir = [string]$rec.shader_cache_dir
                              NbAotDir = [string]$rec.nb_dir; ContentRoot = [string]$rec.content_root; Record = $rec }
}

# The lease taken by Invoke-SrStage -LeaseId ends here (sr_drive's finally: every exit path). -HandoffPid passes it
# to a process that outlives the driver (-KeepAlive: the game still runs from the stage). Never throws; a lease
# that cannot be released lapses when its holder exits, or after 12 h.
function Invoke-SrStageRelease {
    param([Parameter(Mandatory = $true)][string]$Stage, [Parameter(Mandatory = $true)][string]$LeaseId,
          [int]$HandoffPid = 0)
    $py = if ($env:SR_PYTHON) { $env:SR_PYTHON } else { 'python' }
    $pyArgs = @($script:SrStageRollPy, 'release', '--stage', $Stage, '--lease-id', $LeaseId)
    if ($HandoffPid) { $pyArgs += @('--handoff-pid', [string]$HandoffPid) }
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $global:LASTEXITCODE = 0
        $out = (& $py @pyArgs 2>&1 | Out-String).Trim()
        return [pscustomobject]@{ Ok = ($LASTEXITCODE -eq 0); Detail = $out }
    } catch {
        return [pscustomobject]@{ Ok = $false; Detail = $_.Exception.Message }
    } finally {
        $ErrorActionPreference = $prev
    }
}

# After each staged launch: the game becomes a second holder of the driver's lease, so the staged entries stay
# leased while EITHER process lives (a driver killed outright leaves its game protected). Never throws; residual gap:
# a driver killed between the launch and this call.
function Invoke-SrStageLeaseAdd {
    param([Parameter(Mandatory = $true)][string]$Stage, [Parameter(Mandatory = $true)][string]$LeaseId,
          [Parameter(Mandatory = $true)][int]$HolderPid)
    $py = if ($env:SR_PYTHON) { $env:SR_PYTHON } else { 'python' }
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $global:LASTEXITCODE = 0
        $out = (& $py $script:SrStageRollPy 'lease-add' '--stage' $Stage '--lease-id' $LeaseId '--pid' ([string]$HolderPid) 2>&1 |
                Out-String).Trim()
        return [pscustomobject]@{ Ok = ($LASTEXITCODE -eq 0); Detail = $out }
    } catch {
        return [pscustomobject]@{ Ok = $false; Detail = $_.Exception.Message }
    } finally {
        $ErrorActionPreference = $prev
    }
}

# Before each staged launch: the stage volume still has the floor free (stage_roll.py's record: floor + the reserve
# for the game's own writes). Another writer may have filled it since the stage. Never throws.
function Test-SrStageSpace {
    param([Parameter(Mandatory = $true)]$Record)
    try {
        $drive = New-Object System.IO.DriveInfo ([System.IO.Path]::GetPathRoot([string]$Record.stage))
        if (-not $drive.IsReady -or -not (Test-Path -LiteralPath ([string]$Record.stage) -PathType Container)) {
            return [pscustomobject]@{ Ok = $false; Free = -1; Detail = ("the stage {0} is not reachable" -f $Record.stage) }
        }
        $free = [long]$drive.AvailableFreeSpace
        $need = [long]$Record.floor + [long]$Record.reserve
        $ok = $free -ge $need
        return [pscustomobject]@{ Ok = $ok; Free = $free
                                  Detail = ("{0} has {1:N1} GiB free; the floor is {2:N1} GiB + {3:N1} GiB reserve" -f
                                            $drive.Name, ($free / 1GB), ([long]$Record.floor / 1GB), ([long]$Record.reserve / 1GB)) }
    } catch {
        return [pscustomobject]@{ Ok = $false; Free = -1; Detail = ("free space unreadable: " + $_.Exception.Message) }
    }
}

# Provenance written BEFORE the run so it survives a crash. Identity is recorded
# by CONTENT HASH, not by path or timestamp: iter-450W's contamination was only
# detectable by comparing file mtimes, which is not evidence anyone should have
# to reconstruct.
function Write-SrProvenance {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]$Roots,
        [Parameter(Mandatory = $true)][int]$Attempt,
        [Parameter(Mandatory = $true)][string]$ScenarioPath,
        [string]$ScenarioName = '',
        [string]$Gates = '',
        [string]$RequestedDumpDir = '',
        [string]$ExePath = '',
        [int]$TimeoutSeconds = 0,
        [int]$Retries = 0
    )
    # ⛔ `git diff HEAD` DOES NOT SEE UNTRACKED FILES. Deriving worktreeDirty
    # from it alone made provenance assert a clean tree while the run was driven
    # by an untracked scenario (director finding, iter-450Y; measured: git status
    # shows `?? frontend_flat_trace.json` while git diff HEAD for that path
    # returns 0 lines). A manifest that misreports tree state is worse than one
    # that omits it - it launders an unreproducible run as reproducible.
    # `git status --porcelain` covers tracked modifications AND untracked files.
    # -z gives NUL-separated records with no quoting/escaping, and
    # --untracked-files=all expands an untracked DIRECTORY into its individual
    # files. Plain porcelain collapses a whole directory to one entry, which
    # cannot identify an untracked scenario directory (director finding,
    # iter-450Z).
    $statusZ = Invoke-SrGit @('status', '--porcelain', '-z', '--untracked-files=all')
    $status  = Invoke-SrGit @('status', '--porcelain', '--untracked-files=all')
    # --binary so identity covers binary changes too; plain diff omits them.
    $diff    = Invoke-SrGit @('diff', '--binary', 'HEAD')
    # Source identity is COMPLETE only if every git probe succeeded. Otherwise
    # the manifest says so rather than reporting a clean tree by omission.
    $identityOk = $statusZ.Ok -and $status.Ok -and $diff.Ok
    # Untracked files are INVENTORIED AND HASHED, not merely counted: an
    # untracked scenario is part of the run's identity and is absent from the
    # commit, so its content hash is the only way to reproduce the run later.
    $untracked = @()
    foreach ($rec in ($statusZ.Text -split "`0")) {
        if ($rec -notmatch '^\?\?\s') { continue }
        $rel = $rec.Substring(3)
        $full = Join-Path (Get-Location).Path $rel
        $item = $null
        try { $item = Get-Item -LiteralPath $full -Force -ErrorAction Stop } catch { }
        $untracked += [ordered]@{
            path   = $rel
            bytes  = $(if ($item -and -not $item.PSIsContainer) { $item.Length } else { $null })
            sha256 = Get-SrFileHashSafe $full
        }
    }
    $untracked = @($untracked | Sort-Object { $_.path })
    $dirty = -not [string]::IsNullOrWhiteSpace($status.Text)
    $hashOf = {
        param($text)
        if ([string]::IsNullOrWhiteSpace($text)) { return $null }
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            return ([BitConverter]::ToString(
                $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))) -replace '-', '')
        } finally { $sha.Dispose() }
    }
    $diffHash   = & $hashOf $diff.Text
    $statusHash = & $hashOf $status.Text

    # AMBIENT GATES, via a snapshot that cannot throw (see Get-SrEnvSnapshot).
    $envSnap = Get-SrEnvSnapshot
    $srEnv   = $envSnap.Vars

    $commit  = Invoke-SrGit @('rev-parse', 'HEAD')
    $commitS = Invoke-SrGit @('rev-parse', '--short', 'HEAD')
    $branch  = Invoke-SrGit @('rev-parse', '--abbrev-ref', 'HEAD')
    # ⛔ COMPLETENESS IS ABOUT CONTENT, NOT JUST EXIT CODES. A missing executable
    # previously produced sourceIdentityComplete=True with no executable hash,
    # and a non-git directory wrote an "incomplete" manifest that still looked
    # usable (director finding, iter-452). Identity is complete only if every
    # component that identifies the run is actually present.
    $scenarioHash = Get-SrFileHashSafe $ScenarioPath
    $exeHash      = Get-SrFileHashSafe $ExePath
    $untrackedComplete = $true
    foreach ($u in $untracked) {
        if ($null -eq $u.sha256 -or $null -eq $u.bytes) { $untrackedComplete = $false }
    }
    $identityOk = $identityOk -and $commit.Ok -and $commitS.Ok -and $branch.Ok -and $envSnap.Ok `
                  -and [bool]$scenarioHash -and [bool]$exeHash -and $untrackedComplete -and [bool]$commit.Text

    $p = [ordered]@{
        attempt          = $Attempt
        startedUtc       = (Get-Date).ToUniversalTime().ToString('o')
        scenarioPath     = $ScenarioPath
        scenarioName     = $ScenarioName
        scenarioSha256   = $scenarioHash
        exePath          = $ExePath
        exeSha256        = $exeHash
        commitFull       = $commit.Text
        commitShort      = $commitS.Text
        branch           = $branch.Text
        # ⛔ If ANY probe failed, the manifest says so. Without this a failed git
        # produced empty values that persisted as a clean tree - unavailable
        # source identity looking identical to a pristine one.
        sourceIdentityComplete = $identityOk
        untrackedInventoryComplete = $untrackedComplete
        envSnapshotOk    = $envSnap.Ok
        envSnapshotError = $envSnap.Error
        worktreeDirty      = $dirty
        worktreeDiffSha256 = $diffHash          # tracked modifications only
        worktreeStatusSha256 = $statusHash      # tracked AND untracked - the real identity
        untrackedCount     = $untracked.Count
        untrackedFiles     = $untracked
        gates            = $Gates               # what THIS invocation passed
        effectiveSrEnv   = $srEnv               # what was actually in effect, ambient included
        requestedDumpDir = $RequestedDumpDir
        effectiveDumpDir = $Roots.FrameDir
        timeoutSeconds   = $TimeoutSeconds
        retries          = $Retries
        pid              = $null      # filled in by Update-SrProvenanceProcess
        hwnd             = $null
    }
    $p | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Roots.ProvenanceJson -Encoding ascii
    return $p
}

# PID and the VALIDATED HWND are only known after launch, so they are merged in
# rather than guessed up front.
function Update-SrProvenanceProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]$Roots,
        [int]$ProcessId = 0,
        [string]$Hwnd = ''
    )
    if (-not (Test-Path -LiteralPath $Roots.ProvenanceJson)) { return }
    $p = Get-Content -LiteralPath $Roots.ProvenanceJson -Raw | ConvertFrom-Json
    $p.pid  = $ProcessId
    $p.hwnd = $Hwnd
    $p | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Roots.ProvenanceJson -Encoding ascii
}

# Outcome + a SORTED frame inventory with hashes. "How many frames and which
# ones" must be answerable from the manifest alone.
function Write-SrOutcome {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]$Roots,
        [Parameter(Mandatory = $true)][bool]$Ok,
        [string]$FailReason = '',
        [int]$LogLines = 0,
        [switch]$HashFrames
    )
    $frames = @(Get-ChildItem -LiteralPath $Roots.FrameDir -Filter *.bmp -File -ErrorAction SilentlyContinue |
                Sort-Object Name)
    $inv = @()
    foreach ($f in $frames) {
        $inv += [ordered]@{
            name   = $f.Name
            bytes  = $f.Length
            sha256 = $(if ($HashFrames) { Get-SrFileHashSafe $f.FullName } else { $null })
        }
    }
    $o = [ordered]@{
        endedUtc   = (Get-Date).ToUniversalTime().ToString('o')
        result     = $(if ($Ok) { 'SUCCESS' } else { 'FAILURE' })
        failReason = $FailReason
        frameCount = $frames.Count
        logLines   = $LogLines
        frames     = $inv
    }
    $o | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Roots.OutcomeJson -Encoding ascii
    return $o
}
