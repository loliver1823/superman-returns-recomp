# test_sr_drive_harness.ps1 - iter-464 bgruns: the HARNESS KEYBOARD CHANNEL that
# lets sr_drive.ps1 drive a game whose window stays in the background
# (kernel/host_automation.hpp; Open-SrHarnessInput & co. in sr_drive_core.ps1).
#
# No game, no window, no real input. This process plays the game's side of the
# channel with its own named mapping (stamped with ITS PID), so the harness
# functions are driven against the real Win32 object they open in a roll.
#   A. the key tables: SR_KEY_SCANCODES names == SR_KEY_NAMES, scancodes are the
#      SDL ones the game's mapping table reads (spot values), sr_drive's $KEYS
#      gives every key a scancode;
#   B. the channel name mirrors kernel/host_automation.cpp harnessChannelName;
#   C. open/hold/release/read-back/stats against a live mapping; a wrong PID, a
#      wrong magic and a missing channel are REFUSED (never written); a closed
#      channel cannot prove a release (reads as held);
#   D. sr_drive.ps1 wiring by AST: -ForegroundInput exists, harness is the
#      default, Send-Key / Focus-Game / the IsDown probes branch on it, the
#      channel is opened per attempt and closed after finalization.
# Exit code = number of failed checks. ASCII only.

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)
. (Join-Path $PSScriptRoot 'sr_drive_core.ps1')
$pass = 0; $fail = 0
function Check([bool]$ok, [string]$name) {
    if ($ok) { $script:pass++; Write-Host "  ok  $name" }
    else { $script:fail++; Write-Host "FAIL: $name" }
}

# ---- A. key tables ----------------------------------------------------------
$names = @($script:SR_KEY_NAMES | Sort-Object)
$scNames = @($script:SR_KEY_SCANCODES.Keys | Sort-Object)
Check (($names -join ',') -eq ($scNames -join ',')) "A1 SR_KEY_SCANCODES names == SR_KEY_NAMES ($($names.Count))"
# SDL_scancode.h: A=4 ... Z=29, RETURN=40, ESCAPE=41, BACKSPACE=42, F9=66, RIGHT..UP=79..82
$want = @{ ENTER = 40; RETURN = 40; ESCAPE = 41; BACK = 42; RIGHT = 79; LEFT = 80; DOWN = 81; UP = 82
           LB = 20; RB = 8; A = 29; B = 27; X = 6; Y = 25; F9 = 66 }
$bad = @($want.Keys | Where-Object { $script:SR_KEY_SCANCODES[$_] -ne $want[$_] })
Check ($bad.Count -eq 0) ("A2 scancodes are SDL's (Q=20 E=8 Z=29 X=27 C=6 V=25 ...)" + $(if ($bad.Count) { " -- WRONG: $($bad -join ',')" } else { '' }))
$drive = Get-Content 'tools/sr_drive.ps1' -Raw
Check ($drive -match '\$script:VkToSc\[\[int\]\$KEYS\[\$kn\]\.vk\] = \[int\]\$script:SR_KEY_SCANCODES\[\$kn\]') `
    'A3 sr_drive builds vk -> scancode from $KEYS x SR_KEY_SCANCODES'

# ---- B. channel name ----------------------------------------------------------
Check ((Get-SrHarnessChannelName 'run0') -ceq 'Local\SR_HARNESS_INPUT_run0') "B1 run0"
Check ((Get-SrHarnessChannelName '') -ceq 'Local\SR_HARNESS_INPUT_default') "B2 empty -> default"
Check ((Get-SrHarnessChannelName 'a b\c/d:e.f-g_h') -ceq 'Local\SR_HARNESS_INPUT_a_b_c_d_e.f-g_h') "B3 sanitised like the C++ side"

# ---- C. a live channel ------------------------------------------------------------
$inst = "hstest$PID"
$name = Get-SrHarnessChannelName $inst
$game = [System.IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew($name, 4096)
$gv = $game.CreateViewAccessor(0, 4096)
function Stamp([uint32]$magic, [uint32]$ver, [uint32]$owner) {
    $gv.Write([int64]0, $magic); $gv.Write([int64]4, $ver); $gv.Write([int64]8, $owner)
}
try {
    Stamp 0 0 0
    $t0 = Get-Date
    $r = Open-SrHarnessInput $inst $PID 600
    Check ((-not $r.ok) -and ((Get-Date) - $t0).TotalMilliseconds -ge 500) "C1 an unstamped channel is refused after the timeout ($($r.reason))"

    Stamp $script:SR_HARNESS_MAGIC 1 ([uint32]($PID + 1))
    $r = Open-SrHarnessInput $inst $PID 300
    Check ((-not $r.ok) -and $r.reason -match 'pid') "C2 another process's channel is refused ($($r.reason))"

    Stamp 0x12345678 1 ([uint32]$PID)
    $r = Open-SrHarnessInput $inst $PID 300
    Check ((-not $r.ok) -and $r.reason -match 'magic') "C3 a wrong magic is refused"

    $r = Open-SrHarnessInput "nosuch$PID" $PID 300
    Check (-not $r.ok) "C4 a missing channel is refused ($($r.reason))"

    Stamp $script:SR_HARNESS_MAGIC 1 ([uint32]$PID)
    $h = Open-SrHarnessInput $inst $PID 2000
    Check ($h.ok -and $h.name -ceq $name) "C5 the PID-stamped channel opens"

    $sc = $script:SR_KEY_SCANCODES['ENTER']
    Check (-not (Test-SrHarnessKeyDown $h $sc)) "C6 a fresh key reads released"
    Check (Set-SrHarnessKey $h $sc $true) "C7 hold ENTER writes"
    Check ($gv.ReadByte([int64](0x40 + $sc)) -eq 1) "C8 the game side sees byte 0x40+40 = 1"
    Check (Test-SrHarnessKeyDown $h $sc) "C9 read-back says held"
    Check (Set-SrHarnessKey $h $sc $false) "C10 release writes"
    Check ($gv.ReadByte([int64](0x40 + $sc)) -eq 0 -and -not (Test-SrHarnessKeyDown $h $sc)) "C11 released on both sides"
    Check (-not (Set-SrHarnessKey $h 0 $true) -and -not (Set-SrHarnessKey $h 512 $true)) "C12 out-of-range scancodes are refused"

    $gv.Write([int64]0x0C, [uint32]7); $gv.Write([int64]0x10, [uint32]0x0010)
    $s = Get-SrHarnessStats $h
    Check ($s -and $s.polls -eq 7 -and $s.lastMask -eq 0x0010) "C13 stats read the game's merge count + last mask"

    # The release body production uses, bound to the channel: a held key is
    # released and PROVEN released through the read-back.
    [void](Set-SrHarnessKey $h $sc $true)
    $rel = Invoke-VerifiedRelease 0x0D $false 3 @{
        IsDown = { param($k) Test-SrHarnessKeyDown $h $sc }.GetNewClosure()
        SendUp = { param($k, $e) Set-SrHarnessKey $h $sc $false }.GetNewClosure()
        Sleep  = { param($ms) }
        Note   = { param($m) }
    }
    Check ($rel.released -and $gv.ReadByte([int64](0x40 + $sc)) -eq 0) "C14 Invoke-VerifiedRelease releases + proves through the channel"

    Close-SrHarnessInput $h
    Check ((-not (Set-SrHarnessKey $h $sc $true)) -and (Test-SrHarnessKeyDown $h $sc)) "C15 a closed channel cannot write and reads as HELD (no false release proof)"
    $rel = Invoke-VerifiedRelease 0x0D $false 2 @{
        IsDown = { param($k) Test-SrHarnessKeyDown $h $sc }.GetNewClosure()
        SendUp = { param($k, $e) Set-SrHarnessKey $h $sc $false }.GetNewClosure()
        Sleep  = { param($ms) }
        Note   = { param($m) }
    }
    Check (-not $rel.released) "C16 ... so the release body reports the key STUCK"
} finally {
    $gv.Dispose(); $game.Dispose()
}

# ---- D. sr_drive.ps1 wiring (AST) ---------------------------------------------------
$t = $null; $e = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path 'tools/sr_drive.ps1').Path, [ref]$t, [ref]$e)
Check (-not $e -or $e.Count -eq 0) "D0 sr_drive.ps1 parses"
$params = @($ast.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath })
Check ($params -contains 'ForegroundInput') "D1 -ForegroundInput parameter exists"
function FnText([string]$n) {
    $f = $ast.FindAll({ param($x) $x -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $x.Name -eq $n }, $true) | Select-Object -First 1
    if ($f) { return $f.Extent.Text } else { return '' }
}
Check ($drive -match "\`$script:InputMode = \`$\(if \(\`$ForegroundInput\) \{ 'foreground' \} else \{ 'harness' \}\)") "D2 harness is the default input mode"
$sk = FnText 'Send-Key'
Check ($sk -match "InputMode -eq 'harness'" -and $sk -match 'Set-SrHarnessKey' -and $sk -match 'Invoke-SendKeyResult' -and
       $sk.IndexOf('Set-SrHarnessKey') -lt $sk.IndexOf('SendInput(')) "D3 Send-Key writes the channel in harness mode, before any SendInput"
$fg = FnText 'Focus-Game'
Check ($fg -match "InputMode -eq 'harness'\) \{ return \`$true \}" -and $fg.IndexOf("InputMode -eq 'harness'") -lt $fg.IndexOf('SetForegroundWindow')) `
    "D4 Focus-Game returns before SetForegroundWindow in harness mode"
$kd = FnText 'Test-KeyDown'
Check ($kd -match 'Test-SrHarnessKeyDown' -and $kd -match 'GetAsyncKeyState') "D5 Test-KeyDown reads the channel in harness mode, the OS otherwise"
$probeDowns = [regex]::Matches($drive, 'IsDown\s*=\s*\{[^}]*\}') | ForEach-Object { $_.Value }
Check (@($probeDowns).Count -ge 2 -and @($probeDowns | Where-Object { $_ -notmatch 'Test-KeyDown' }).Count -eq 0) `
    "D6 every production IsDown probe goes through Test-KeyDown ($(@($probeDowns).Count))"
Check ($drive -match 'Open-SrHarnessInput \$env:SR_INSTANCE_ID \$proc\.Id') "D7 the channel is opened for the launched PID"
$iFin = $drive.IndexOf('$fin = Invoke-CleanupAndFinalize'); $iClose = $drive.IndexOf('Close-SrHarnessInput $script:Harness')
Check ($iFin -gt 0 -and $iClose -gt $iFin) "D8 the channel is closed only AFTER the cleanup/finalization release check"
Check ($drive -match '\$env:SR_WINDOW_BACKGROUND = "0"; \$env:SR_HARNESS_INPUT = "0"; \$env:SR_HARNESS_REAL_INPUT = "1"' -and
       $drive -match 'SetEnvironmentVariable\(\$n, \$script:InputEnvSaved\[\$n\]\)') "D9 -ForegroundInput sets the black-box env and restores the caller's at the end"

Write-Host ("test_sr_drive_harness: {0} passed, {1} failed" -f $pass, $fail)
exit $fail
