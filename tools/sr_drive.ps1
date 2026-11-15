# sr_drive.ps1 - external UI automation harness for Superman Returns (iter-450V).
#
# WHAT THIS IS: HOST TEST INFRASTRUCTURE. It drives the real game from the outside
# by holding keys, as a human would. It NEVER writes guest memory, calls guest
# functions, fabricates game state, or touches progress flags - the guest still
# receives input only through the normal platform path (mergeKeyboardPad ->
# XamInputGetState). Nothing here ships in the game; the production build has no
# autoplay.
#
# iter-464 bgruns: AUTOMATED RUNS NEVER TAKE THE FOREGROUND (the user works on this
# PC while rolls run). By default the game (SR_INSTANCE_ID set) opens its window
# without activation at the bottom of the z-order, and keys go through the HARNESS
# KEYBOARD CHANNEL (kernel/host_automation.hpp; Open-SrHarnessInput in
# sr_drive_core.ps1): a named file mapping of SDL-scancode bytes that the game maps
# through the same keyboard->pad table as a physical key. No focus, no
# SetForegroundWindow, no SendInput; the game ignores the real keyboard and pads.
# -ForegroundInput restores the old black-box path (SetForegroundWindow + SendInput,
# foreground window, real input) for a test that must prove the OS input path.
#
# WHY IT EXISTS: every era past PRESS START is only reachable through real input,
# and the iter-450U* acquisitions each burned a whole 300 s roll on a human being
# at the keyboard at the right second. This makes the path repeatable and, more
# importantly, CONFIRMS EVERY PULSE rather than assuming it: SR_KB_PAD_OBS reports
# whether the key actually reached the guest, so a press discarded for lack of
# window focus is a NAMED failure instead of a silent one. That exact failure was
# observed live in iter-450UK.
#
# DESIGN RULES (each one paid for in a lost arc):
#   - guarded conditions, never blind sleeps, wherever a signal exists;
#   - key-up is guaranteed in a finally block - a stuck key poisons everything after;
#   - the HWND is re-validated before every action and between every poll;
#   - each step names precondition / action / expected observation / timeout;
#   - artifacts are retained on success AND failure, attempt-tagged, under -Out;
#   - the log is read INCREMENTALLY. A 300 s roll is ~1M lines; re-scanning it at
#     4 Hz with Select-String cannot keep up and would silently miss transitions.
#
# ASCII ONLY. PowerShell 5.1 reads a BOM-less .ps1 as cp1252, so a stray em dash
# becomes a string delimiter and the parse error points at an innocent line.
param(
    [Parameter(Mandatory=$true)][string] $Scenario,   # JSON scenario path
    [Parameter(Mandatory=$true)][string] $Out,        # artifact dir (never repo root)
    [string] $Gates          = "",                    # NAME=VALUE,... extra env gates
    [int]    $TimeoutSeconds = 420,                   # hard cap on one attempt
    [int]    $Retries        = 0,                     # bounded SEQUENTIAL retries (DOGMA)
    [switch] $KeepAlive,                              # leave the game running at the end
    [switch] $ValidateOnly,                           # validate the scenario contract and exit (0 ok / 2 fail); touches NO process, dir or env
    [string] $ExePath        = "",                    # Astra 088: game executable (default build\SupermanReturns.exe); see Resolve-SrGameExe
    [switch] $ForegroundInput,                        # iter-464: black-box input (SetForegroundWindow + SendInput, foreground window, real input) instead of the background harness channel
    [string] $SkipCutscenes  = "auto",                # iter-467: auto | on | off - run the scenario's cutsceneSkip steps (sr_drive_core.ps1 Resolve-SrCutsceneSkip)
    [string] $Stage          = "",                    # 2026-10-14 roll-stage: an SSD folder (J:\sr_stage): the game's READ set is staged there (tools/perf/stage_roll.py) and the staged exe launched; outputs stay where they are
    [string] $StageSource    = "",                    # -Stage: the folder holding "Superman Returns (USA)" to stage from (default: the game's own rule - the exe folder, its parent, the repo root)
    [switch] $GracefulQuit                            # tools/graceful-quit (TOOLKIT 1.8m): end the roll by ASKING the game to quit (tools/sr_quit.ps1) - it drains, prints its terminal snapshot and exits - and fall back to Stop-Process only if that fails; also SR_DRIVE_GRACEFUL_QUIT=1 (env or -Gates)
    # ⛔ NO -Fresh, NO -Reuse. iter-450X shipped both and the director refuted
    # both: -Reuse reset numbering to attempt1 and overwrote that attempt's
    # provenance/outcome/failure/frames - the very collision it claimed to
    # prevent - and -Fresh recursively deleted a caller-supplied path with no
    # safe-root constraint. A non-empty -Out now simply fails closed.
)

$ErrorActionPreference = "Stop"

# step 4 (Fable ruling 13, 2026-10-20): SR_STUCK_HOLD names the hold file the close's stuck watch writes when it catches a
# stuck load. While it exists this driver launches nothing (exit 6, here: before any other action). Seen at ANY point in a
# run, it latches for the rest of the run: no further step, pulse, wait, launch or retry; our own game is never stopped
# (nor asked to quit) and keeps the stage lease if alive; exit 6 on every path. Unset = no hold.
# Known limits (documented, not modelled): a bounded sleep, key hold or exit wait already in progress runs out (at most
# its step's own timeout) before the step stops; a restarted child is adopted by its logged PID + exe path, so PID reuse
# by another copy of the same exe within the restart is not caught (game_slot keeps one game per machine).
$script:HoldFile = [string]$env:SR_STUCK_HOLD
$script:HoldSeen = $false    # the hold file was seen during this run (sticky)
$script:HeldAlive = $false   # ... while our own game was alive: it is never stopped, and keeps the stage lease
$script:HoldNoted = $false
$script:Leftover = $false
function Test-SrStuckHold {
  if ($script:HoldFile -and (Test-Path -LiteralPath $script:HoldFile)) { $script:HoldSeen = $true }
  return $script:HoldSeen
}
# Latches the hold; $true = $p (our own game) must NOT be stopped. Every termination of our own child asks this first.
function Test-SrHoldLatch($p) {
  if ((Test-SrStuckHold) -and $p -and -not $p.HasExited) { $script:HeldAlive = $true }
  return $script:HeldAlive
}
# Puts the hold into this attempt's verdict, once (called before and after the teardown).
function Set-SrHoldVerdict($p) {
  [void](Test-SrHoldLatch $p)
  if (-not $script:HoldSeen -or $script:HoldNoted) { return }
  $script:HoldNoted = $true
  $script:ok = $false
  $why = $(if ($script:HeldAlive) { "the game PID $($p.Id) is left alive for the probe" } else { "the game was not running" })
  $script:failReason = ("SR_STUCK_HOLD {0}: a stuck load was caught - {1}, nothing killed{2}" -f $script:HoldFile, $why,
                        $(if ($script:failReason) { " (" + $script:failReason + ")" } else { "" }))
  Note $script:failReason
}
if (Test-SrStuckHold) {
  [Console]::Error.WriteLine("sr_drive: SR_STUCK_HOLD " + $script:HoldFile + " exists - a stuck game is held; refusing to launch (nothing touched)")
  exit 6
}

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'sr_drive_core.ps1')   # iter-455l hardened-dialog body + validator
. (Join-Path $PSScriptRoot 'sr_quit.ps1')         # tools/graceful-quit: Invoke-SrGracefulQuit (functions only)
if (-not (Test-Path $Scenario)) { Write-Error "sr_drive: no scenario at $Scenario"; exit 2 }

# ---------------------------------------------------------------------------
# iter-455l r4 (director defect 1): scenario parse, hardened validation and
# the -ValidateOnly exit run FIRST - before Add-Type or any other
# initialization. In Codex's restricted review environment Add-Type's
# compiler write under %TEMP% failed and took -ValidateOnly down with it;
# ordering validation first makes the switch test ONLY the validator.
# Fail-closed exit 2. A scenario is subject to the contract if it carries any
# singleDeadline step OR names itself 'hardened' (so deleting singleDeadline
# from the hardened scenario is a validation failure, not a silent downgrade
# to the legacy path).
# ---------------------------------------------------------------------------
$scn = Get-Content $Scenario -Raw | ConvertFrom-Json
# iter-463 (devtools item 6): the general contract, for EVERY scenario (field
# names, keys, regexes, integers, expectExitCode last, requires). Fail-closed.
$gv = Assert-SrScenario $scn
if (-not $gv.ok) {
  [Console]::Error.WriteLine(("sr_drive: scenario validation FAILED: {0}" -f $gv.reason))
  exit 2
}
Write-Host ("sr_drive: scenario '{0}' validated ({1} step(s))" -f $scn.name, @($scn.steps).Count)
# iter-467 forcing: cutscene skipping (sr_drive_core.ps1 Resolve-SrCutsceneSkip): decided once, before any action.
$script:SkipCut = Resolve-SrCutsceneSkip $SkipCutscenes $scn $Scenario ([string]$scn.gates) $Gates $env:SR_DRIVE_SKIP_CUTSCENES
$nSkipSteps = @($scn.steps | Where-Object { $_.cutsceneSkip }).Count
Write-Host ("sr_drive: cutscene skipping {0} ({1}); {2} cutscene-skip step(s)" -f $(if ($script:SkipCut.on) { "ON" } else { "OFF" }), $script:SkipCut.why, $nSkipSteps)
# requires.contentSeed: the seed this invocation WOULD use (scenario gates, then
# -Gates, then the environment). -ValidateOnly reports it; a real run refuses
# below, before any directory, env or process action.
$script:RequiredSeed = Get-SrSeedFromGates ([string]$scn.gates) $Gates $env:SR_CONTENT_DIR
$reqv = Test-SrScenarioRequirements $scn $script:RequiredSeed
Write-Host ("sr_drive: requirements: {0}{1}" -f $(if ($reqv.ok) { "" } else { "NOT MET - " }), $reqv.reason)
$script:HardenedPlan = $null
if (($scn.name -match 'hardened') -or (@($scn.steps | Where-Object { $_.singleDeadline }).Count -gt 0)) {
  $v = Assert-HardenedScenario $scn
  if (-not $v.ok) {
    [Console]::Error.WriteLine(("sr_drive: hardened-scenario validation FAILED: {0}" -f $v.reason))
    exit 2
  }
  $script:HardenedPlan = $v
  Write-Host ("sr_drive: hardened scenario validated (await='{0}' settle='{1}' clear='{2}')" -f $v.awaitName, $v.settleName, $v.clearName)
}
if ($ValidateOnly) { Write-Host "sr_drive: ValidateOnly - stopping before any process, directory or env action"; exit 0 }
if (-not $reqv.ok) {
  [Console]::Error.WriteLine(("sr_drive: scenario requirements NOT MET - refusing before any directory, env or process action: {0}" -f $reqv.reason))
  exit 2
}

# iter-464 bgruns: Start-SrGameProcess (launch with no console window). Loaded
# AFTER -ValidateOnly, like every Add-Type (r4 director defect 1).
. (Join-Path $PSScriptRoot 'bg_window.ps1')

# ---------------------------------------------------------------------------
# Win32: focus + synthetic input. SendInput rather than keybd_event so the scan
# code is explicit - SDL keys off scancodes and a vk-only event can map wrong.
# ---------------------------------------------------------------------------
if (-not ("SRWin32" -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class SRWin32 {
  [StructLayout(LayoutKind.Sequential)]
  public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
  // x64 INPUT: DWORD type, 4 bytes of padding, then the 24-byte union, 40 bytes
  // total. KEYBDINPUT is the smallest arm, so the tail is padded explicitly.
  [StructLayout(LayoutKind.Sequential)]
  public struct INPUT64 { public uint type; public uint pad0; public KEYBDINPUT ki; public ulong pad1; }
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT64[] p, int cb);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint uCode, uint uMapType);
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
  [DllImport("user32.dll")] public static extern IntPtr SetActiveWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr h);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  public const uint KEYEVENTF_KEYUP = 0x0002;
  public const uint KEYEVENTF_EXTENDEDKEY = 0x0001;
  // iter-464: the `mouse` step (docs/MOUSE_CURSOR.md 8). x64 INPUT with the MOUSEINPUT arm:
  // DWORD type, 4 bytes padding, then dx, dy, mouseData, dwFlags, time, 4 bytes padding,
  // dwExtraInfo = 40 bytes, the same size as INPUT64.
  [StructLayout(LayoutKind.Sequential)]
  public struct MINPUT64 { public uint type; public uint pad0; public int dx; public int dy; public uint mouseData;
                           public uint dwFlags; public uint time; public uint pad1; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
  [DllImport("user32.dll", SetLastError=true, EntryPoint="SendInput")] public static extern uint SendMouseInput(uint n, MINPUT64[] p, int cb);
  [DllImport("user32.dll", SetLastError=true)] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
  public const uint MOUSEEVENTF_LEFTDOWN = 0x0002, MOUSEEVENTF_LEFTUP = 0x0004,
                    MOUSEEVENTF_RIGHTDOWN = 0x0008, MOUSEEVENTF_RIGHTUP = 0x0010,
                    MOUSEEVENTF_MIDDLEDOWN = 0x0020, MOUSEEVENTF_MIDDLEUP = 0x0040,
                    MOUSEEVENTF_WHEEL = 0x0800;
}
"@
}

# iter-464: mouse input for the `mouse` step. Coordinates are Apt STAGE pixels (1280x720),
# mapped onto the window's client area, so a scenario names the same point at any window
# size. The cursor is moved with SetCursorPos in small steps (SDL needs real motion events,
# xrel != 0); buttons and the wheel go through SendInput.
function Send-MouseEvent([uint32]$flags, [int]$data = 0) {
  $i = New-Object SRWin32+MINPUT64
  $i.type = 0
  $i.dwFlags = $flags
  $i.mouseData = [uint32]([int64]$data -band 0xFFFFFFFF)
  $arr = [SRWin32+MINPUT64[]]@($i)
  $cb = [System.Runtime.InteropServices.Marshal]::SizeOf([type]("SRWin32+MINPUT64"))
  $n = [SRWin32]::SendMouseInput(1, $arr, $cb)
  if ($n -ne 1) { $script:InputHealth.sendFailures++; return $false }
  return $true
}
function Get-StageScreenPoint([IntPtr]$hwnd, [double]$sx, [double]$sy) {
  $rc = New-Object SRWin32+RECT
  if (-not [SRWin32]::GetClientRect($hwnd, [ref]$rc)) { return $null }
  $w = [Math]::Max(1, $rc.Right - $rc.Left); $h = [Math]::Max(1, $rc.Bottom - $rc.Top)
  $pt = New-Object SRWin32+POINT
  $pt.X = [int][Math]::Round($sx * $w / 1280.0); $pt.Y = [int][Math]::Round($sy * $h / 720.0)
  if (-not [SRWin32]::ClientToScreen($hwnd, [ref]$pt)) { return $null }
  return $pt
}
# Safety: a button or wheel event goes to whatever window is under the cursor, and a human
# may be using the desktop. Every event is sent only while the game window is foreground
# AND is the top-level window under the cursor; otherwise the script stops.
function Test-MouseOnGame([IntPtr]$hwnd) {
  if ([SRWin32]::GetForegroundWindow() -ne $hwnd) { return $false }
  $pt = New-Object SRWin32+POINT
  if (-not [SRWin32]::GetCursorPos([ref]$pt)) { return $false }
  $w = [SRWin32]::WindowFromPoint($pt)
  if ($w -eq [IntPtr]::Zero) { return $false }
  return ([SRWin32]::GetAncestor($w, 2) -eq $hwnd)    # GA_ROOT
}
$script:MousePos = $null
function Move-MouseStage([IntPtr]$hwnd, [double]$sx, [double]$sy, [int]$steps = 8, [int]$stepMs = 15) {
  $to = Get-StageScreenPoint $hwnd $sx $sy
  if ($null -eq $to) { return $false }
  $from = $script:MousePos
  if ($null -eq $from) { $from = New-Object SRWin32+POINT; $from.X = $to.X - 12; $from.Y = $to.Y - 12 }
  for ($k = 1; $k -le $steps; $k++) {
    $x = [int][Math]::Round($from.X + ($to.X - $from.X) * $k / $steps)
    $y = [int][Math]::Round($from.Y + ($to.Y - $from.Y) * $k / $steps)
    [void][SRWin32]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds $stepMs
  }
  $script:MousePos = $to
  return $true
}
# iter-464 HARNESS FEED (default for automated rolls; kernel/mouse_ui.hpp mouseUiPollFeed):
# the commands are appended to the attempt's mouse_feed.txt, which the exe reads every pump
# and turns into the SDL events a real mouse would produce. The desktop cursor never moves
# and the game window needs no focus for the mouse (USER 2026-10-01).
$script:MouseFeed = $null
$script:FeedPos = $null
function Write-FeedLine([string]$line) {
  for ($k = 0; $k -lt 20; $k++) {
    try { [System.IO.File]::AppendAllText($script:MouseFeed, $line + "`n"); return $true }
    catch { Start-Sleep -Milliseconds 25 }
  }
  return $false
}
function Move-FeedStage([double]$sx, [double]$sy, [int]$steps = 8, [int]$stepMs = 15) {
  $from = $script:FeedPos
  if ($null -eq $from) { $from = @(($sx - 12), ($sy - 12)) }
  for ($k = 1; $k -le $steps; $k++) {
    $x = [Math]::Max(0.0, [Math]::Min(1280.0, [double]($from[0] + ($sx - $from[0]) * $k / $steps)))
    $y = [Math]::Max(0.0, [Math]::Min(720.0, [double]($from[1] + ($sy - $from[1]) * $k / $steps)))
    if (-not (Write-FeedLine ([string]::Format([cultureinfo]::InvariantCulture, "move {0:0.#} {1:0.#}", $x, $y)))) { return $false }
    Start-Sleep -Milliseconds $stepMs
  }
  $script:FeedPos = @($sx, $sy)
  return $true
}
function Invoke-FeedCommand([string]$verb, $pts, $parts) {
  $btn = @{ click = 'left'; rclick = 'right'; mclick = 'middle' }
  switch ($verb) {
    'move'   { if (-not (Move-FeedStage $pts[0][0] $pts[0][1])) { return "feed write failed" } }
    { $_ -in @('click', 'rclick', 'mclick') } {
               if ($pts.Count) { if (-not (Move-FeedStage $pts[0][0] $pts[0][1])) { return "feed write failed" } }
               Start-Sleep -Milliseconds 120
               [void](Write-FeedLine ("down " + $btn[$verb])); Start-Sleep -Milliseconds 90
               [void](Write-FeedLine ("up " + $btn[$verb])) }
    'wheel'  { $nN = [int]$parts[1]
               for ($k = 0; $k -lt [Math]::Abs($nN); $k++) {
                 [void](Write-FeedLine ("wheel {0}" -f [Math]::Sign($nN))); Start-Sleep -Milliseconds 150 } }
    'drag'   { if (-not (Move-FeedStage $pts[0][0] $pts[0][1])) { return "feed write failed" }
               Start-Sleep -Milliseconds 120
               [void](Write-FeedLine "down left")
               try {
                 Start-Sleep -Milliseconds 400
                 [void](Move-FeedStage $pts[1][0] $pts[1][1] 24 60)
                 Start-Sleep -Milliseconds 1500
               } finally { [void](Write-FeedLine "up left") } }
    'wait'   { $deadline = (Get-Date).AddMilliseconds([int]$parts[1])
               while ((Get-Date) -lt $deadline) { Pump-Log; Start-Sleep -Milliseconds 100 } }
    default  { return "unknown mouse verb '$verb'" }
  }
  return $null
}

# One `mouse` script: commands separated by ';' (grammar: Test-SrMouseScript in
# sr_drive_core.ps1). Returns @{ ok; reason }.
function Invoke-MouseScript([IntPtr]$hwnd, [string]$text, [string]$name) {
  foreach ($raw in $text.Split(';')) {
    $c = $raw.Trim()
    if (-not $c) { continue }
    $parts = @($c -split ' +')
    $verb = $parts[0].ToLower()
    $pts = @()
    foreach ($a in @($parts | Select-Object -Skip 1)) {
      if ($a -match '^(-?[0-9.]+),(-?[0-9.]+)$') { $pts += ,@([double]$Matches[1], [double]$Matches[2]) }
    }
    if ($verb -in @('moveitem', 'clickitem')) {
      $side = $(if ($parts.Count -ge 3) { $parts[2].ToLower() } else { 'center' })
      $pt = Get-MouseItemPoint ([int]$parts[1]) $side
      if ($null -eq $pt) {
        return @{ ok = $false; reason = ("'{0}': no item index {1} in the last [MOUSE_UI] snapshot ('{2}'; the gates need SR_MOUSE_UI_LOG=1 and a move first)" -f $c, $parts[1], $script:MouseWhat) }
      }
      Note ([string]::Format([cultureinfo]::InvariantCulture, "step '{0}': {1} -> ({2:0.#},{3:0.#}) in '{4}'", $name, $c, $pt[0], $pt[1], $script:MouseWhat))
      $verb = $(if ($verb -eq 'moveitem') { 'move' } else { 'click' })
      $pts = @(,$pt)
      $c = [string]::Format([cultureinfo]::InvariantCulture, "{0} {1:0.#},{2:0.#}", $verb, $pt[0], $pt[1])
      $parts = @($verb, ([string]::Format([cultureinfo]::InvariantCulture, "{0:0.#},{1:0.#}", $pt[0], $pt[1])))
    }
    if ($verb -in @('movehint', 'clickhint')) {   # lane E E5: aim at a button hint of the last snapshot
      Pump-Log
      $pt = Get-SrHintPoint $script:MouseHints $parts[1]
      if ($null -eq $pt) {
        return @{ ok = $false; reason = ("'{0}': no {1} hint in the last [MOUSE_UI] snapshot ('{2}'; the gates need SR_MOUSE_UI_LOG=1 and the custom front end, and a move first)" -f $c, $parts[1].ToUpper(), $script:MouseWhat) }
      }
      Note ([string]::Format([cultureinfo]::InvariantCulture, "step '{0}': {1} -> ({2:0.#},{3:0.#}) in '{4}'", $name, $c, $pt[0], $pt[1], $script:MouseWhat))
      $verb = $(if ($verb -eq 'movehint') { 'move' } else { 'click' })
      $pts = @(,$pt)
      $c = [string]::Format([cultureinfo]::InvariantCulture, "{0} {1:0.#},{2:0.#}", $verb, $pt[0], $pt[1])
      $parts = @($verb, ([string]::Format([cultureinfo]::InvariantCulture, "{0:0.#},{1:0.#}", $pt[0], $pt[1])))
    }
    if ($script:MouseFeed) {
      Note ("step '{0}': mouse {1} (feed)" -f $name, $c)
      $err = Invoke-FeedCommand $verb $pts $parts
      if ($err) { return @{ ok = $false; reason = $err } }
      Pump-Log
      continue
    }
    Note ("step '{0}': mouse {1}" -f $name, $c)
    # A human is back at the desk if the cursor is no longer where this script left it:
    # stop at once rather than fight them for the mouse (USER 2026-10-01: rolls must not
    # take the desktop from the user).
    if ($null -ne $script:MousePos) {
      $cur = New-Object SRWin32+POINT
      if ([SRWin32]::GetCursorPos([ref]$cur) -and
          ([Math]::Abs($cur.X - $script:MousePos.X) -gt 4 -or [Math]::Abs($cur.Y - $script:MousePos.Y) -gt 4)) {
        return @{ ok = $false; reason = ("the desktop cursor moved to {0},{1} (left at {2},{3}): a human is using the mouse - mouse script stopped" -f $cur.X, $cur.Y, $script:MousePos.X, $script:MousePos.Y) }
      }
    }
    if ($verb -in @('click', 'rclick', 'mclick', 'wheel', 'drag')) {
      if ($verb -eq 'drag' -or $pts.Count) { [void](Move-MouseStage $hwnd $pts[0][0] $pts[0][1]) }
      if (-not (Test-MouseOnGame $hwnd)) {
        [void](Focus-Game $hwnd)
        if (-not (Test-MouseOnGame $hwnd)) { return @{ ok = $false; reason = "the cursor is not over the foreground game window; '$c' not sent" } }
      }
    }
    switch ($verb) {
      'move'   { if (-not (Move-MouseStage $hwnd $pts[0][0] $pts[0][1])) { return @{ ok = $false; reason = "move failed" } } }
      'click'  { if ($pts.Count) { [void](Move-MouseStage $hwnd $pts[0][0] $pts[0][1]) }
                 Start-Sleep -Milliseconds 120
                 [void](Send-MouseEvent ([SRWin32]::MOUSEEVENTF_LEFTDOWN)); Start-Sleep -Milliseconds 90
                 [void](Send-MouseEvent ([SRWin32]::MOUSEEVENTF_LEFTUP)) }
      'rclick' { if ($pts.Count) { [void](Move-MouseStage $hwnd $pts[0][0] $pts[0][1]) }
                 [void](Send-MouseEvent ([SRWin32]::MOUSEEVENTF_RIGHTDOWN)); Start-Sleep -Milliseconds 90
                 [void](Send-MouseEvent ([SRWin32]::MOUSEEVENTF_RIGHTUP)) }
      'mclick' { if ($pts.Count) { [void](Move-MouseStage $hwnd $pts[0][0] $pts[0][1]) }
                 [void](Send-MouseEvent ([SRWin32]::MOUSEEVENTF_MIDDLEDOWN)); Start-Sleep -Milliseconds 90
                 [void](Send-MouseEvent ([SRWin32]::MOUSEEVENTF_MIDDLEUP)) }
      'wheel'  { $nN = [int]$parts[1]
                 for ($k = 0; $k -lt [Math]::Abs($nN); $k++) {
                   [void](Send-MouseEvent ([SRWin32]::MOUSEEVENTF_WHEEL) ([Math]::Sign($nN) * 120)); Start-Sleep -Milliseconds 150 } }
      'drag'   { [void](Move-MouseStage $hwnd $pts[0][0] $pts[0][1])
                 Start-Sleep -Milliseconds 120
                 [void](Send-MouseEvent ([SRWin32]::MOUSEEVENTF_LEFTDOWN))
                 try {
                   Start-Sleep -Milliseconds 400
                   [void](Move-MouseStage $hwnd $pts[1][0] $pts[1][1] 24 60)
                   Start-Sleep -Milliseconds 1500
                 } finally { [void](Send-MouseEvent ([SRWin32]::MOUSEEVENTF_LEFTUP)) } }
      'wait'   { $deadline = (Get-Date).AddMilliseconds([int]$parts[1])
                 while ((Get-Date) -lt $deadline) { Pump-Log; Start-Sleep -Milliseconds 100 } }
      default  { return @{ ok = $false; reason = "unknown mouse verb '$verb'" } }
    }
    Pump-Log
  }
  return @{ ok = $true; reason = 'ok' }
}

# Virtual key + the pad bit kernel/stubs.cpp:5089-5108 maps it to. The mask is
# what makes a pulse CONFIRMABLE: [KBPAD_OBS] prints mappedMask per poll, so we
# assert the guest saw the exact button we sent, not merely "some input".
#
# ⛔ `ext` is NOT optional on the arrow keys. The four arrows are EXTENDED keys:
# their scan codes (0x48/0x50/0x4B/0x4D) are shared with the numeric keypad, and
# only the extended bit tells them apart. iter-450V sent VK_DOWN without it and
# the guest never saw a single press - SDL resolved scancode 0x50 to KP_2, which
# maps to no pad button, so four A presses landed on the mode page and both DOWN
# presses vanished. That read as "the menu ignores the D-pad" until the flag was
# found. Same applies to Insert/Delete/Home/End/PgUp/PgDn/RCtrl/RAlt if added.
$KEYS = @{
  "ENTER"  = @{ vk = 0x0D; mask = 0x0010; ext = $false }   # START
  "RETURN" = @{ vk = 0x0D; mask = 0x0010; ext = $false }
  "ESCAPE" = @{ vk = 0x1B; mask = 0x0020; ext = $false }   # BACK
  "BACK"   = @{ vk = 0x08; mask = 0x0020; ext = $false }
  "UP"     = @{ vk = 0x26; mask = 0x0001; ext = $true  }
  "DOWN"   = @{ vk = 0x28; mask = 0x0002; ext = $true  }
  "LEFT"   = @{ vk = 0x25; mask = 0x0004; ext = $true  }
  "RIGHT"  = @{ vk = 0x27; mask = 0x0008; ext = $true  }
  "LB"     = @{ vk = 0x51; mask = 0x0100; ext = $false }   # Q
  "RB"     = @{ vk = 0x45; mask = 0x0200; ext = $false }   # E
  "A"      = @{ vk = 0x5A; mask = 0x1000; ext = $false }   # Z
  "B"      = @{ vk = 0x58; mask = 0x2000; ext = $false }   # X
  "X"      = @{ vk = 0x43; mask = 0x4000; ext = $false }   # C
  "Y"      = @{ vk = 0x56; mask = 0x8000; ext = $false }   # V
  "F9"     = @{ vk = 0x78; mask = 0;      ext = $false }   # host trace hotkey
  # iter-464 DT-2: the free camera's keys (docs/DEVTOOLS.md 6.4). F6 toggles it (freecam tool on);
  # while it is on, WASD / IJKL / R / F are guest controller 2's sticks and triggers. No button
  # bit (mask 0), so no KBPAD_OBS confirm: a step that needs proof uses 'expect'.
  "F6"     = @{ vk = 0x75; mask = 0;      ext = $false }   # freecam on / off
  "F7"     = @{ vk = 0x76; mask = 0;      ext = $false }   # warp Superman to the free camera (cheats + freecam)
  "F8"     = @{ vk = 0x77; mask = 0;      ext = $false }   # infinite_stamina on / off (cheats)
  "KEY_W"  = @{ vk = 0x57; mask = 0;      ext = $false }   # left stick up (move forward)
  "KEY_A"  = @{ vk = 0x41; mask = 0;      ext = $false }   # left stick left
  "KEY_S"  = @{ vk = 0x53; mask = 0;      ext = $false }   # left stick down
  "KEY_D"  = @{ vk = 0x44; mask = 0;      ext = $false }   # left stick right
  "KEY_I"  = @{ vk = 0x49; mask = 0;      ext = $false }   # freecam: right stick up (look)
  "KEY_J"  = @{ vk = 0x4A; mask = 0;      ext = $false }   # freecam: right stick left
  "KEY_K"  = @{ vk = 0x4B; mask = 0;      ext = $false }   # freecam: right stick down
  "KEY_L"  = @{ vk = 0x4C; mask = 0;      ext = $false }   # freecam: right stick right
  "KEY_R"  = @{ vk = 0x52; mask = 0;      ext = $false }   # freecam: RT (up)
  "KEY_F"  = @{ vk = 0x46; mask = 0;      ext = $false }   # freecam: LT (down)
  # iter-465 modlib: the mod library's own keys (docs/MODS.md "The mod library": Space = toggle,
  # Tab = next tab, PgUp / PgDn = category, F12 = SR_MOD_LIBRARY_SHOTS capture). No pad bit.
  "SPACE"    = @{ vk = 0x20; mask = 0;    ext = $false }
  "TAB"      = @{ vk = 0x09; mask = 0;    ext = $false }
  "PAGEUP"   = @{ vk = 0x21; mask = 0;    ext = $true  }
  "PAGEDOWN" = @{ vk = 0x22; mask = 0;    ext = $true  }
  "F12"      = @{ vk = 0x7B; mask = 0;    ext = $false }
}
# iter-463: the scenario validator's key list (sr_drive_core.ps1 SR_KEY_NAMES)
# must be exactly this table, or a validated key could still be unknown here.
if ((@($KEYS.Keys | Sort-Object) -join ',') -ne (@($script:SR_KEY_NAMES | Sort-Object) -join ',')) {
  [Console]::Error.WriteLine("sr_drive: the key table and SR_KEY_NAMES in sr_drive_core.ps1 differ - refusing")
  exit 2
}

# iter-464 bgruns: the input path. 'harness' (default): keys are bytes in the
# game's harness keyboard channel - the window stays in the background and needs
# no focus. 'foreground' (-ForegroundInput): the old SetForegroundWindow +
# SendInput path. vk -> SDL scancode comes from the core's SR_KEY_SCANCODES.
$script:InputMode = $(if ($ForegroundInput) { 'foreground' } else { 'harness' })
$script:Harness = $null
$script:VkToSc = @{}
foreach ($kn in $KEYS.Keys) { $script:VkToSc[[int]$KEYS[$kn].vk] = [int]$script:SR_KEY_SCANCODES[$kn] }

$script:HeldVks = New-Object System.Collections.ArrayList
# iter-456q: input-delivery health. Both are reported in the run summary so a
# roll that could not deliver input is visible WITHOUT reading the game log.
# ⚠ ONE CANONICAL RECORD, mutated in place and read once at finalization. Two
# separate counters read at different moments is how a cleanup failure went
# unseen: the snapshot was taken before the unwind that produced it.
$script:InputHealth = @{ sendFailures = 0; stuckKeys = 0 }

function Send-Key([int]$vk, [bool]$up, [bool]$ext = $false) {
  # iter-464 bgruns: harness mode holds/releases the key's byte in the game's
  # channel - no focus, no SendInput. Failure is recorded by the SAME adapter
  # (Invoke-SendKeyResult), so input health is judged identically in both modes.
  if ($script:InputMode -eq 'harness') {
    $written = Set-SrHarnessKey $script:Harness $script:VkToSc[$vk] (-not $up)
    if (-not $written) { Note ("HARNESS KEY WRITE FAILED for vk=0x{0:X2} (channel {1})" -f $vk, $(if ($script:Harness) { $script:Harness.name } else { 'not open' })) }
    return (Invoke-SendKeyResult $(if ($written) { 1 } else { 0 }) 0 $vk $up $script:InputHealth { param($m) Note $m })
  }
  $scan = [SRWin32]::MapVirtualKey([uint32]$vk, 0)
  $i = New-Object SRWin32+INPUT64
  $i.type = 1
  $ki = New-Object SRWin32+KEYBDINPUT
  $ki.wVk = [uint16]$vk
  $ki.wScan = [uint16]$scan
  $flags = [uint32]0
  if ($up)  { $flags = $flags -bor [SRWin32]::KEYEVENTF_KEYUP }
  if ($ext) { $flags = $flags -bor [SRWin32]::KEYEVENTF_EXTENDEDKEY }
  $ki.dwFlags = $flags
  $ki.time = 0
  $ki.dwExtraInfo = [IntPtr]::Zero
  $i.ki = $ki
  $arr = [SRWin32+INPUT64[]]@($i)
  $cb = [System.Runtime.InteropServices.Marshal]::SizeOf([type]("SRWin32+INPUT64"))
  # iter-456q: THE RESULT IS CHECKED. It was discarded with [void], so a
  # SendInput blocked by UIPI (a more-privileged foreground window) or by a
  # low-level hook returned 0 and the driver still reported the press as sent.
  # The DllImport already carried SetLastError=true; nothing ever used it.
  $n = [SRWin32]::SendInput(1, $arr, $cb)
  $err = if ($n -ne 1) { [System.Runtime.InteropServices.Marshal]::GetLastWin32Error() } else { 0 }
  # iter-456q: the DECISION lives in the core adapter, so a test can drive the
  # real one with n=0 instead of hand-incrementing the record and hoping this
  # half agrees.
  return (Invoke-SendKeyResult $n $err $vk $up $script:InputHealth { param($m) Note $m })
}

# iter-464 bgruns: is the key down, in the input path this run uses? Harness
# mode reads the channel byte back (a dead channel reads as held, so a release
# is never proven through it); foreground mode asks the OS (GetAsyncKeyState).
function Test-KeyDown([int]$vk) {
  if ($script:InputMode -eq 'harness') { return (Test-SrHarnessKeyDown $script:Harness $script:VkToSc[$vk]) }
  return ((([SRWin32]::GetAsyncKeyState($vk)) -band 0x8000) -ne 0)
}

# iter-456q: a release is ESTABLISHED, not assumed. GetAsyncKeyState's high bit
# is the down state; the key-up is resent until it clears, and a key that cannot
# be proven released FAILS LOUDLY rather than silently poisoning every step
# after it - which is the driver's own stated design rule, previously unenforced.
function Assert-KeyReleased([int]$vk, [bool]$ext, [int]$attempts = 5) {
  # iter-456q: a thin binding over the core's injectable body, so the release
  # LOGIC is covered by the self-test rather than only by a live roll.
  $r = Invoke-VerifiedRelease $vk $ext $attempts @{
    IsDown = { param($k) Test-KeyDown $k }
    SendUp = { param($k, $e) Send-Key $k $true $e }
    Sleep  = { param($ms) Start-Sleep -Milliseconds $ms }
    Note   = { param($m) Note $m }
  }
  # ⚠ NOT `+= $r.sendFailures`. Send-Key already increments the counter at the
  # point of failure, so adding the body's own tally here counted every release
  # failure TWICE - which would have made the new fatal check fire at double
  # strength and misreport how bad a roll actually was.
  # iter-456q: NO per-step stuckKeys increment. stuckKeys is DERIVED at
  # finalization from the keys still unreleased; incrementing here counted one
  # physical key twice when cleanup found it still held.
  return $r.released
}

# iter-456q: the sustained-black gate wrapper. The LOGIC lives in
# sr_drive_core.ps1 as an injectable body so the self-test can drive it on a
# virtual clock; this only binds the real probes.
#
# ⚠ THIS FUNCTION WAS DELETED ONCE, by an anchor-based edit that sliced from
# Assert-KeyReleased to Release-AllKeys without checking what sat between them.
# The call site survived, so the acquisition would have failed at
# observe-gameplay instead of applying the gate - and every helper test still
# passed, because they drive the core directly and never the production wiring.
# tools/test_sr_drive_wiring.ps1 now asserts, by AST, that every command
# sr_drive.ps1 invokes is actually defined.
function Wait-SustainedBlack([string]$frameDir, [datetime]$notBefore,
                             [int]$requireMs, [int]$timeoutMs) {
  # iter-456q: the probes come from the CORE builder, so the frameDir and
  # notBefore bindings are the same code the self-test exercises. Building them
  # inline here is where a wrong directory or a forgotten post-entry filter
  # would hide, unseen by every test.
  $probes = New-BlackGateProbes $frameDir $notBefore `
    { param($path) Test-FrameExactBlack $path } `
    { [int64]([DateTime]::UtcNow - [DateTime]'1970-01-01').TotalMilliseconds } `
    { param($ms) Pump-Log; Start-Sleep -Milliseconds $ms } `
    { param($m) Note $m }
  $r = Invoke-SustainedBlackGate $requireMs $timeoutMs $probes
  return $r.ok
}

# iter-458 (Astra 038): the mirror gate - "captured VISIBLE before any input".
# Same core probe builder, so the frameDir / notBefore bindings are the tested ones.
function Wait-SustainedVisible([string]$frameDir, [datetime]$notBefore,
                               [int]$requireMs, [int]$timeoutMs) {
  $probes = New-BlackGateProbes $frameDir $notBefore `
    { param($path) Test-FrameExactBlack $path } `
    { [int64]([DateTime]::UtcNow - [DateTime]'1970-01-01').TotalMilliseconds } `
    { param($ms) Pump-Log; Start-Sleep -Milliseconds $ms } `
    { param($m) Note $m }
  $r = Invoke-SustainedVisibleGate $requireMs $timeoutMs $probes
  return $r.ok
}

# iter-456q: the production probe set, defined ONCE so finalization composes the
# same adapters a live run uses rather than a hand-built imitation.
function New-ProductionKeyProbes {
  @{
    SendUp = { param($vk, $ext) Send-Key $vk $true $ext }
    IsDown = { param($vk) Test-KeyDown $vk }
    Sleep  = { param($ms) Start-Sleep -Milliseconds $ms }
    Note   = { param($m) Note $m }
  }
}

# Windows REFUSES SetForegroundWindow from a process that does not already own
# the foreground. iter-450W lost a 13-minute run to exactly that: the operator
# was using the machine, the call was silently denied, and the step aborted with
# "could not foreground HWND". The documented way through is to AttachThreadInput
# to whichever thread currently owns the foreground - that puts us in its input
# queue, so the foreground lock no longer applies to us - then detach again.
# Retried with backoff, because a transient foreground steal is normal on a
# desktop a human is also using.
function Focus-Game([IntPtr]$hwnd, [int]$attempts = 10) {
  if (-not [SRWin32]::IsWindow($hwnd)) { return $false }
  # iter-464 bgruns: the harness keyboard needs no focus - the window is never
  # touched and stays in the background while the user works.
  if ($script:InputMode -eq 'harness') { return $true }
  $me = [SRWin32]::GetCurrentThreadId()
  for ($i = 0; $i -lt $attempts; $i++) {
    if ([SRWin32]::GetForegroundWindow() -eq $hwnd) { return $true }
    $fg = [SRWin32]::GetForegroundWindow()
    $fgPid = 0
    $fgThread = if ($fg -ne [IntPtr]::Zero) { [SRWin32]::GetWindowThreadProcessId($fg, [ref]$fgPid) } else { 0 }
    $attached = $false
    if ($fgThread -ne 0 -and $fgThread -ne $me) {
      $attached = [SRWin32]::AttachThreadInput($me, $fgThread, $true)
    }
    try {
      [void][SRWin32]::ShowWindow($hwnd, 9)      # SW_RESTORE
      [void][SRWin32]::BringWindowToTop($hwnd)
      [void][SRWin32]::SetForegroundWindow($hwnd)
      [void][SRWin32]::SetActiveWindow($hwnd)
      [void][SRWin32]::SetFocus($hwnd)
    } finally {
      if ($attached) { [void][SRWin32]::AttachThreadInput($me, $fgThread, $false) }
    }
    for ($w = 0; $w -lt 6; $w++) {
      if ([SRWin32]::GetForegroundWindow() -eq $hwnd) { return $true }
      Start-Sleep -Milliseconds 50
    }
    Start-Sleep -Milliseconds (100 * ($i + 1))   # backoff
  }
  return $false
}

# ---------------------------------------------------------------------------
# Incremental log reader. Every pattern the scenario mentions is registered up
# front and counted from the first byte, so an `await` can match something that
# happened before its step began while a `confirm` can still ask "since mark".
# ---------------------------------------------------------------------------
$script:LogPath   = $null
$script:Stream    = $null
$script:Reader    = $null
$script:Counts    = @{}
$script:Regex     = @{}
$script:LastHit   = @{}
$script:LinesSeen = 0

function Register-Pattern([string]$p) {
  if (-not $p) { return }
  if ($script:Counts.ContainsKey($p)) { return }
  $script:Counts[$p]  = 0
  $script:Regex[$p]   = [regex]::new($p, [System.Text.RegularExpressions.RegexOptions]::Compiled)
  $script:LastHit[$p] = ""
}

function Pump-Log {
  if (-not $script:Reader) {
    if (-not (Test-Path $script:LogPath)) { return }
    try {
      $script:Stream = [System.IO.File]::Open($script:LogPath, [System.IO.FileMode]::Open,
                        [System.IO.FileAccess]::Read,
                        [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
      $script:Reader = New-Object System.IO.StreamReader($script:Stream)
    } catch { $script:Reader = $null; return }
  }
  $keys = @($script:Regex.Keys)
  while ($true) {
    $line = $script:Reader.ReadLine()
    if ($null -eq $line) { break }
    $script:LinesSeen++
    foreach ($p in $keys) {
      if ($script:Regex[$p].IsMatch($line)) {
        $script:Counts[$p]++
        $script:LastHit[$p] = $line
      }
    }
    if ($line.Contains('[MOUSE_UI]')) { Update-MouseItems $line }
  }
}

# iter-464 mouse: the item rects of the LATEST [MOUSE_UI] snapshot (SR_MOUSE_UI_LOG=1 logs
# them on every change of the menu, its selection or its layout). `moveitem` / `clickitem`
# aim at an item by the game's own index, so a scenario does not hard-code a page layout.
$script:MouseItems = @{}
$script:MouseHints = @{}   # lane E E5: the snapshot's button hints by AS key code (movehint / clickhint)
$script:MouseWhat = ''
function Update-MouseItems([string]$line) {
  $script:MouseHints = Update-SrHintSet $script:MouseHints $line   # lane E E5 (sr_drive_core.ps1)
  if ($line -match "\[MOUSE_UI\] snapshot #\d+ .*what='([^']*)'") {
    $script:MouseItems = @{}
    $script:MouseWhat = $Matches[1]
    return
  }
  if ($line -match '\[MOUSE_UI\]\s+slot \d+ index=(-?\d+) kind=(\d+) en=(\d) rect=\((-?[0-9.]+),(-?[0-9.]+)\)-\((-?[0-9.]+),(-?[0-9.]+)\)') {
    $ci = [cultureinfo]::InvariantCulture
    $script:MouseItems[[int]$Matches[1]] = @{
      kind = [int]$Matches[2]; en = [int]$Matches[3]
      x0 = [double]::Parse($Matches[4], $ci); y0 = [double]::Parse($Matches[5], $ci)
      x1 = [double]::Parse($Matches[6], $ci); y1 = [double]::Parse($Matches[7], $ci) }
  }
}
# The stage point of item <index> (side: left / center / right of its rect), or $null.
function Get-MouseItemPoint([int]$index, [string]$side) {
  Pump-Log
  if (-not $script:MouseItems.ContainsKey($index)) { return $null }
  $it = $script:MouseItems[$index]
  $f = switch ($side) { 'left' { 0.2 } 'right' { 0.8 } default { 0.5 } }
  $x = $it.x0 + ($it.x1 - $it.x0) * $f
  $y = 0.5 * ($it.y0 + $it.y1)
  return ,@([Math]::Max(0.0, [Math]::Min(1280.0, $x)), [Math]::Max(0.0, [Math]::Min(720.0, $y)))
}

function Count-Of([string]$p) { return [int]$script:Counts[$p] }

# Guarded wait: pump the log, then test. NEVER a blind sleep.
# 2026-10-18: every wait also watches the game ($script:WatchProc, set at the launch, cleared while a MODS-menu
# restart hands over, set to the child after it) and returns as soon as it has exited (Invoke-WaitBody,
# sr_drive_core.ps1): s4h_wg_rel1's crash at +533 s left a 7,740 s precondition wait running. $script:WaitExit
# names the exit when the last wait ended on one ($null otherwise); the step's failure reason carries it.
$script:WatchProc = $null
$script:WaitExit = $null
function Wait-For([scriptblock]$cond, [int]$timeoutMs, [int]$pollMs = 200) {
  $wfProc = $script:WatchProc
  $wfCond = $cond
  $r = Invoke-WaitBody $timeoutMs $pollMs @{
    NowMs = { [int64]([DateTime]::UtcNow.Ticks / 10000) }
    # Fable ruling 13: once a hold is seen (latched) no condition counts as met, so the hold always ends the wait.
    Cond  = { if ($script:HoldFile -and (Test-Path -LiteralPath $script:HoldFile)) { $script:HoldSeen = $true }
              if ($script:HoldSeen) { $false } else { & $wfCond } }
    Alive = { if ($script:HoldSeen) { "is held: SR_STUCK_HOLD exists (a stuck load was caught)" }
              elseif ($wfProc -and $wfProc.HasExited) { Get-SrAliveReason $true $wfProc.ExitCode $true 0 } else { $null } }
    Pump  = { Pump-Log }
    Sleep = { param($ms) Start-Sleep -Milliseconds $ms }
  }
  $script:WaitExit = $(if ($r.exited) { $r.reason } else { $null })
  if ($r.exited) { Note ("wait ended early: the game {0} {1} ms into a {2} ms wait" -f $r.reason, $r.elapsedMs, $timeoutMs) }
  return [bool]$r.ok
}
function Get-WaitExitNote { if ($script:WaitExit) { " - the game $($script:WaitExit) while waiting" } else { "" } }

# ---------------------------------------------------------------------------
# ($scn was parsed and validated at the top of the script - r4 defect 1.)

# ---------------------------------------------------------------------------
# ⛔ ARTIFACT COLLISION SAFETY (iter-450X, director finding on iter-450W).
# This script used to create -Out with -Force, truncate only steps.log, APPEND
# failure_reason.txt, and let frame dumps land in the same directory. Point two
# invocations at one -Out and the bank silently MIXES them: archive/
# iter450w_default1 ended up holding a focus failure AND a later settle-title
# failure, with p000000 stamped 21:34 while higher-numbered frames still read
# 21:30. Those 39 frames and the sheet built from them were invalid evidence,
# and nothing in the output said so. Frame filenames restart at 000 every run,
# so only the copied run logs were ever attempt-tagged.
#
# Rules now, ALL enforced before any process is launched OR terminated:
#   1. A non-empty -Out is REFUSED, full stop. There is no -Fresh and no -Reuse.
#   2. Every attempt writes to its OWN attempt<N>/ - frames, steps.log,
#      provenance, outcome, failure reason and run log all inside it - and
#      allocation refuses a directory that already exists.
# The preflight and allocation live in tools/sr_artifacts.ps1 so tests can
# exercise them WITHOUT launching or killing the game.
# ---------------------------------------------------------------------------
. (Join-Path $PSScriptRoot 'sr_artifacts.ps1')

# Astra 088 (Option B): resolve the game executable ONCE, before the artifact
# root is claimed and before any process action. A bad -ExePath is refused here
# (exit 3), leaving no directory behind and touching no process. The resolved
# object is the single source for hashing/provenance, the sweep and the launch.
$gameExe = Resolve-SrGameExe -Root $root -ExePath $ExePath
if (-not $gameExe.Ok) { [Console]::Error.WriteLine("sr_drive: " + $gameExe.Reason); exit 3 }
if ($StageSource -and -not $Stage) { [Console]::Error.WriteLine("sr_drive: -StageSource needs -Stage"); exit 3 }

# ⛔ ORDER MATTERS: this refusal must happen before the Stop-Process sweep and
# before Start-Process. A refused invocation must not kill the operator's
# running game as a side effect of being refused.
$pre = Test-SrArtifactRoot -Out $Out
if (-not $pre.Ok) {
  # NOT Write-Error: this script sets $ErrorActionPreference='Stop', so
  # Write-Error throws and exits 1 before reaching `exit 3`, and callers could
  # not distinguish "refused to mix banks" from any other failure.
  [Console]::Error.WriteLine($pre.Reason)
  exit 3
}
# Claim atomically. Preflight alone left a check-then-create window in which two
# drivers could both pass and both allocate attempt1.
$claim = Lock-SrArtifactRoot -Out $Out
if (-not $claim.Ok) { [Console]::Error.WriteLine($claim.Reason); exit 3 }

# Campaign-level log covers only the lines emitted before the first attempt is
# allocated; everything after is attempt-owned. -Out is guaranteed empty above,
# so this can never truncate a previous invocation's journal.
$journal = Join-Path $Out "campaign.log"
Set-Content -Path $journal -Value "" -Encoding ascii
function Note([string]$m) {
  $line = ("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $m)
  Write-Host $line
  # step 4 (2026-10-20): Add-Content opened the journal without read sharing, so any reader (a watcher, tail, an
  # editor) holding it at that instant failed the whole drive. Append with read/write/delete sharing (same bytes:
  # ASCII + CRLF) and retry a transient sharing violation; the path resolves against PowerShell's location as before.
  $path = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($journal)
  $bytes = [Text.Encoding]::ASCII.GetBytes($line + "`r`n")
  for ($i = 0; ; $i++) {
    try {
      $fs = [IO.File]::Open($path, [IO.FileMode]::Append, [IO.FileAccess]::Write,
                            [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
      try { $fs.Write($bytes, 0, $bytes.Length) } finally { $fs.Dispose() }
      break
    } catch [IO.IOException] {
      if ($i -ge 40) { throw }
      Start-Sleep -Milliseconds 25
    }
  }
}

$exeName = $gameExe.Name
$exePath = $gameExe.Path
$sweepNames = @($gameExe.SweepNames)
# $script:LogPath is set after the gates are applied (below), from SR_INSTANCE_ID.

Note ("scenario '{0}': {1} step(s); out={2}" -f $scn.name, @($scn.steps).Count, $Out)
Note ("executable: {0} (process '{1}', default={2}, sweep: {3})" -f $exePath, $exeName, $gameExe.IsDefault, ($sweepNames -join ","))

# ---------------------------------------------------------------------------
# INVOCATION GATES ARE APPLIED ONCE, HERE, BEFORE ANY ATTEMPT.
# They were applied inside the retry loop, which meant the caller's requested
# SR_PRESENT_DUMP_DIR was captured BEFORE a -Gates/scenario value could set it -
# so a dump dir supplied that way never reached provenance at all (director
# finding, iter-451). Apply gates once, then capture the request once.
# ---------------------------------------------------------------------------
$allGates = @()
if ($scn.gates) { $allGates += ([string]$scn.gates) }
if ($Gates)     { $allGates += $Gates }
foreach ($chunk in $allGates) {
  foreach ($pair in $chunk.Split(",")) {
    $kv = $pair.Split("=", 2)
    if ($kv.Count -eq 2 -and $kv[0].Trim()) {
      Set-Item -Path ("Env:" + $kv[0].Trim()) -Value $kv[1].Trim()
    }
  }
}
# iter-464: honour a caller-set SR_INSTANCE_ID (env or -Gates) so a lane's rolls get their own
# log file and harness channel; "run0" stays the default. A roll that shared run0 with another
# lane's roll (2026-10-02 slot violation) also shared superman_returns_run0.log and the channel.
if (-not $env:SR_INSTANCE_ID) { $env:SR_INSTANCE_ID = "run0" }
$script:LogPath = Join-Path $root ("superman_returns_{0}.log" -f $env:SR_INSTANCE_ID)
Note ("instance: SR_INSTANCE_ID={0} log={1}" -f $env:SR_INSTANCE_ID, $script:LogPath)
# tools/graceful-quit (TOOLKIT 1.8m): how the roll ENDS. Opt-in (-GracefulQuit, or SR_DRIVE_GRACEFUL_QUIT=1 from the
# environment or -Gates, read after the gates are applied); -KeepAlive leaves the game running either way.
$script:GracefulQuit = [bool]$GracefulQuit -or ($env:SR_DRIVE_GRACEFUL_QUIT -eq '1')
# The bounds (sr_quit.ps1 Get-SrQuitEnvMs): digits in 1..600000 ms, else the default - never -1 (= wait forever).
$quitAckEnv = Get-SrQuitEnvMs 'SR_DRIVE_QUIT_ACK_MS' 5000
$quitExitEnv = Get-SrQuitEnvMs 'SR_DRIVE_QUIT_EXIT_MS' 45000
$script:QuitAckMs = $quitAckEnv.value
$script:QuitExitMs = $quitExitEnv.value
if ($quitAckEnv.bad -or $quitExitEnv.bad) {
  Note ("WARN: SR_DRIVE_QUIT_ACK_MS / SR_DRIVE_QUIT_EXIT_MS outside 1..600000 ms or not a number - the defaults are used (ack {0} ms, exit {1} ms)" -f $script:QuitAckMs, $script:QuitExitMs)
}
Note ("end: {0}" -f $(if ($KeepAlive) { "-KeepAlive (the game is left running)" } elseif ($script:GracefulQuit) { "GRACEFUL QUIT (ack <= $($script:QuitAckMs) ms, exit <= $($script:QuitExitMs) ms, then Stop-Process)" } else { "Stop-Process -Force (-GracefulQuit asks the game to quit with a terminal snapshot)" }))
# iter-464: a scenario with `mouse` steps drives the exe's harness mouse feed (per attempt,
# below) unless a gate says SR_MOUSE_FEED=desktop (the real cursor, needs an idle desktop).
$script:ScenarioHasMouse = [bool](@($scn.steps | Where-Object { $_.mouse }).Count)
$script:MouseFeedMode = $(if ($env:SR_MOUSE_FEED -eq 'desktop') { 'desktop' } else { 'feed' })
Remove-Item Env:SR_MOUSE_FEED -ErrorAction SilentlyContinue
# iter-463 phase 2: no dev tool unless a gate names one (tools/devtools/skip_run.py pins it too).
if ($null -eq $env:SR_DEVTOOLS) { $env:SR_DEVTOOLS = "0" }
# iter-464 bgruns: the input path. Harness mode leaves the game's automated-run
# defaults alone (background window, harness channel, real input ignored).
# -ForegroundInput asks for the old black-box shape; the caller's values are put
# back at the end so a later roll in the same session does not inherit them.
$script:InputEnvSaved = @{}
if ($script:InputMode -eq 'foreground') {
  foreach ($n in @('SR_WINDOW_BACKGROUND', 'SR_HARNESS_INPUT', 'SR_HARNESS_REAL_INPUT')) {
    $script:InputEnvSaved[$n] = [Environment]::GetEnvironmentVariable($n)
  }
  $env:SR_WINDOW_BACKGROUND = "0"; $env:SR_HARNESS_INPUT = "0"; $env:SR_HARNESS_REAL_INPUT = "1"
  Note "input: FOREGROUND (-ForegroundInput): SetForegroundWindow + SendInput; the game window comes to the front and reads the real keyboard"
} else {
  Note "input: HARNESS keyboard channel - the game window stays in the background, no focus, no SendInput (-ForegroundInput for the black-box path)"
}
$script:HasKeySteps = @($scn.steps | Where-Object { $_.key }).Count -gt 0
# iter-464 mouse: the REAL cursor (SR_MOUSE_FEED=desktop) is real input, which an automated
# run ignores unless it is the foreground black-box run (kernel/host_automation.hpp
# realInput) - so the desktop mouse path requires -ForegroundInput.
if ($script:ScenarioHasMouse -and $script:MouseFeedMode -eq 'desktop' -and $script:InputMode -ne 'foreground') {
  $m = "sr_drive: SR_MOUSE_FEED=desktop drives the real cursor, which a background roll ignores (input isolation): add -ForegroundInput, or drop the gate to use the harness mouse feed"
  Note $m
  [Console]::Error.WriteLine($m)
  exit 3
}

# 2026-10-14 roll-stage (owner: "okay do them on j just be wary about space and resources"): -Stage copies
# what the game READS - the exe folder, the disc, the installed shader cache - to an SSD folder and launches
# the STAGED exe. The working directory stays $root, so the log, frames, perf dir, saves and dumps land where
# they do without -Stage. Staged AFTER the gates are applied (a gate's SR_SHADER_CACHE_DIR is what gets staged;
# SR_MODS / SR_DEVTOOLS refuse) and BEFORE any process action; a refusal exits 3 having touched no process.
# The sweep covers the staged path AND the source path, so a stale game of either survives into no roll.
# The staged entries are LEASED to this driver from inside the stage lock until the finally below (every exit
# path; -KeepAlive hands the lease to the game), so no clean or retention can remove them under the roll.
$script:SweepPaths = @($exePath)
$script:StageLeaseId = $null
$script:StageRecord = $null
if ($Stage) {
  $leaseId = "sr_drive-{0}-{1}" -f $PID, [DateTime]::UtcNow.Ticks
  $st = Invoke-SrStage -Stage $Stage -ExePath $exePath -Root $root -Source $StageSource -LeaseId $leaseId `
          -JsonPath (Join-Path $Out 'stage.json') -Note { param($l) Note ("stage: " + $l) }
  if (-not $st.Ok) {
    $m = "sr_drive: -Stage refused - no process touched: " + $st.Reason
    Note $m
    [Console]::Error.WriteLine($m)
    [void](Invoke-SrStageRelease -Stage $Stage -LeaseId $leaseId)
    exit 3
  }
  $script:StageLeaseId = $leaseId
  $script:StageRecord = $st.Record
  $script:SweepPaths += $st.ExePath
  $exePath = $st.ExePath
  if ($st.ShaderCacheDir) { $env:SR_SHADER_CACHE_DIR = $st.ShaderCacheDir }
  if ($st.NbAotDir) { $env:SR_NB_AOT_DIR = $st.NbAotDir }
  Note ("stage: launching the staged exe {0} (content root {1}; SR_SHADER_CACHE_DIR={2}); working directory {3}; lease {4}" -f
        $exePath, $st.ContentRoot, $env:SR_SHADER_CACHE_DIR, $root, $leaseId)
}

try {   # 2026-10-14 roll-stage: the finally at the end releases the stage lease on every exit path
# The caller's ORIGINAL SR_PRESENT_DUMP_DIR - captured AFTER gates are applied
# (so a gate-supplied value counts) and BEFORE any attempt overwrites it.
$script:OriginalDumpDir = $env:SR_PRESENT_DUMP_DIR
# iter-463 SAVES ARE HERMETIC: a caller-supplied SR_CONTENT_DIR (env, -Gates or
# scenario gates) is a read-only SEED, sampled once like the dump dir.
$script:ContentSeed = $env:SR_CONTENT_DIR

$attempt = 0
$ok = $false

while ($attempt -le $Retries -and -not $ok) {
  $attempt++
  $failReason = $null
  $proc = $null
  $script:WatchProc = $null; $script:WaitExit = $null
  $hwnd = [IntPtr]::Zero

  # Every attempt owns a disjoint, immutable root. Frame filenames restart at
  # 000 each run, so without this a retry's p000000 overwrites the previous
  # attempt's - precisely how iter450w_default1's bank became unreadable.
  # New-SrAttemptRoot REFUSES an existing directory rather than writing into it.
  $roots      = New-SrAttemptRoot -Out $Out -Attempt $attempt
  $attemptDir = $roots.AttemptDir
  $frameDir   = $roots.FrameDir
  # From here on the journal is attempt-owned; the campaign log only ever holds
  # the pre-attempt lines.
  # soft_failures.txt was the last campaign-shared outcome artifact; an
  # optional-step failure from attempt 1 would otherwise be indistinguishable
  # from one in attempt 2 (director finding, iter-450Y).
  $softFailuresTxt = Join-Path $roots.AttemptDir "soft_failures.txt"
  $journal = $roots.StepsLog
  Set-Content -Path $journal -Value "" -Encoding ascii
  # The banner is emitted AFTER journal reassignment. Emitting it before meant
  # a retry's "=== ATTEMPT 2 ===" line was appended to attempt 1's steps.log,
  # so the journals disagreed about which attempt they described (director
  # finding, iter-450Y).
  Note ("=== ATTEMPT {0} of {1} ===" -f $attempt, ($Retries + 1))

  # iter-463: set by an `expectExitCode` step whose process exited as required.
  $script:ExitedAsExpected = $false
  # iter-465: how many RESTART NOW relaunches this attempt has followed (followRestart).
  $script:RestartGen = 0
  # Fresh counters per attempt - a retry must not inherit the last run's hits.
  $script:Counts = @{}; $script:Regex = @{}; $script:LastHit = @{}; $script:LinesSeen = 0
  # 2026-10-14: the line the latest `expect` observed (a step's `onlyIfObserved` tests it)
  $script:LastObserved = ""
  # 2026-10-15: step boundaries (`expectSince`): which steps arm one, and the snapshots armed in this attempt
  $script:ArmPlan = Get-SrArmPlan $scn.steps
  $script:Armed = @{}
  if ($script:Reader) { try { $script:Reader.Dispose() } catch {}; $script:Reader = $null }
  if ($script:Stream) { try { $script:Stream.Dispose() } catch {}; $script:Stream = $null }
  foreach ($s in $scn.steps) {
    Register-Pattern $s.await
    Register-Pattern $s.expect
    Register-Pattern $s.confirm
    if ($s.key) {
      $kd = $KEYS[$s.key.ToUpper()]
      if ($kd -and $kd.mask -gt 0 -and -not $s.confirm) {
        Register-Pattern ("KBPAD_OBS.*mappedMask=0x{0:X4}" -f $kd.mask)
      }
    }
  }

  try {
    # ---- exactly one visible instance ------------------------------------
    # Frames ALWAYS land in this attempt's own directory. A caller-supplied
    # SR_PRESENT_DUMP_DIR is overridden rather than honoured: honouring it is
    # what let two invocations share one bank. The requested value is sampled
    # ONCE before the loop, because attempt 2 used to read attempt 1's frame dir
    # back and record it as the caller's request (director finding, iter-450Z).
    $requestedDumpDir = $script:OriginalDumpDir
    if ($requestedDumpDir -and ((Resolve-Path -LiteralPath $requestedDumpDir -ErrorAction SilentlyContinue).Path -ne
                                (Resolve-Path -LiteralPath $frameDir -ErrorAction SilentlyContinue).Path)) {
      Note ("gate override: SR_PRESENT_DUMP_DIR '{0}' -> '{1}' (attempt-scoped, collision safety)" -f $requestedDumpDir, $frameDir)
    }
    $env:SR_PRESENT_DUMP_DIR = $frameDir
    New-Item -ItemType Directory -Force $env:SR_PRESENT_DUMP_DIR | Out-Null

    # iter-463: every attempt owns a fresh save root inside its attempt dir, so
    # a roll never reads the player's saves or a previous attempt's save (the
    # natural game enumerates saves at boot and a found save changes the front
    # end path). A seed is copied in, never written. Kept as evidence.
    $contentDir = Join-Path $roots.AttemptDir "content"
    New-Item -ItemType Directory -Force $contentDir | Out-Null
    if ($script:ContentSeed -and (Test-Path -LiteralPath $script:ContentSeed)) {
      Copy-Item -Path (Join-Path $script:ContentSeed "*") -Destination $contentDir -Recurse -Force
    }
    $env:SR_CONTENT_DIR = $contentDir
    $script:MouseFeed = $null
    $script:FeedPos = $null
    if ($script:ScenarioHasMouse -and $script:MouseFeedMode -eq 'feed') {
      $script:MouseFeed = Join-Path $roots.AttemptDir "mouse_feed.txt"
      [System.IO.File]::WriteAllText($script:MouseFeed, "")
      $env:SR_MOUSE_FEED = $script:MouseFeed
      Note ("mouse: harness feed {0} (the desktop cursor is never moved)" -f $script:MouseFeed)
    }
    Note ("saves: SR_CONTENT_DIR={0} ({1})" -f $contentDir, $(if ($script:ContentSeed) { "seeded from " + $script:ContentSeed } else { "empty" }))

    # ⛔ SOURCE IDENTITY IS ESTABLISHED BEFORE ANY PROCESS ACTION.
    # This used to run AFTER the Stop-Process sweep, and Get-ChildItem Env:
    # throws when the environment holds both PATH and Path - so the first live
    # invocation in such an environment would KILL THE OPERATOR'S GAME and then
    # die without launching a replacement (director finding, iter-450Z).
    # It is also wrapped: a manifest failure must never abort a run, least of
    # all one that has already terminated something.
    # ⛔ FAILURE ENDS THE ATTEMPT, BEFORE ANY PROCESS IS TOUCHED.
    # The previous version logged "continuing; the manifest is incomplete" and
    # then went on to Stop-Process/Start-Process - so a serialization, hash or
    # filesystem failure could still launch a run with no trustworthy
    # provenance. "Evidence identity before process action" is a CONTRACT, not
    # a source ordering: if identity cannot be established, there is no run
    # (director finding, iter-451).
    try {
      $prov = (Write-SrProvenance -Roots $roots -Attempt $attempt `
               -ScenarioPath (Resolve-Path $Scenario).Path -ScenarioName $scn.name `
               -Gates ($allGates -join ",") -RequestedDumpDir $requestedDumpDir `
               -ExePath $exePath -TimeoutSeconds $TimeoutSeconds -Retries $Retries)
    } catch {
      $failReason = "provenance could not be established: $($_.Exception.Message) - refusing to launch"
      Note ("ABORTING ATTEMPT (no process touched): {0}" -f $failReason)
      $stepOk = $false
      throw $failReason
    }
    # ⛔ A manifest that WROTE SUCCESSFULLY but reports incomplete identity is
    # just as disqualifying as one that threw. The driver used to discard the
    # returned object, so a git failure or a missing executable produced a
    # written-but-untrustworthy manifest and the run proceeded anyway (director
    # finding, iter-452).
    if (-not $prov -or -not $prov.sourceIdentityComplete) {
      $failReason = "source identity INCOMPLETE (sourceIdentityComplete=false) - refusing to launch"
      Note ("ABORTING ATTEMPT (no process touched): {0}" -f $failReason)
      $stepOk = $false
      throw $failReason
    }

    # 2026-10-14 roll-stage: the stage volume must still hold its floor before EACH launch (a retry included):
    # another writer may have filled it since the stage authorised the first.
    if ($script:StageRecord) {
      $sp = Test-SrStageSpace -Record $script:StageRecord
      if (-not $sp.Ok) {
        $failReason = "stage: " + $sp.Detail + " - below the floor, refusing to launch"
        Note ("ABORTING ATTEMPT (no process touched): {0}" -f $failReason)
        $stepOk = $false
        throw $failReason
      }
    }

    # ---- only NOW may we touch processes ---------------------------------
    # iter-464: only copies of THIS executable (same full path) count. Another lane's game (a
    # different snapshot exe) is never ours; tools/game_slot.py keeps one game on the machine,
    # and a by-name sweep took down a concurrent roll (2026-10-02).
    # step 4 (Fable ruling 13, 2026-10-20): DETECT AND REFUSE - a path match is not ownership,
    # so nothing is killed here. A copy of this exe already running is a leftover (none in a
    # correct run; a held stuck game is exactly what the stuck watch preserves): it is logged,
    # nothing is touched, and the run fails with exit 5 and no retry. The driver stops only its
    # own child, by the PID it launched.
    $leftovers = @()
    Get-Process -Name $sweepNames -ErrorAction SilentlyContinue | ForEach-Object {
      $pp = $null; try { $pp = $_.Path } catch {}
      if ($pp -and @($script:SweepPaths | Where-Object { [string]::Equals([IO.Path]::GetFullPath($pp),
                       [IO.Path]::GetFullPath($_), [StringComparison]::OrdinalIgnoreCase) }).Count) {
        $ppid = '?'; $started = '?'
        try { $ppid = (Get-CimInstance Win32_Process -Filter ("ProcessId = {0}" -f $_.Id) -ErrorAction Stop).ParentProcessId } catch {}
        try { $started = $_.StartTime.ToString('yyyy-MM-dd HH:mm:ss.fff') } catch {}
        $leftovers += ("PID {0} (parent {1}, started {2}) {3}" -f $_.Id, $ppid, $started, $pp)
      } else {
        Note ("sweep: left PID {0} alone (not this exe: {1})" -f $_.Id, $pp)
      }
    }
    if ($leftovers.Count) {
      $script:Leftover = $true
      $failReason = ("a copy of this exe is already running - refusing to launch, nothing killed: {0}" -f ($leftovers -join '; '))
      Note ("ABORTING ATTEMPT (no process touched): {0}" -f $failReason)
      $stepOk = $false
      throw $failReason
    }
    Start-Sleep -Milliseconds 500
    if (Test-Path $script:LogPath) { try { Remove-Item $script:LogPath -Force } catch {} }
    # iter-465: a relaunched child's log from an earlier roll must never satisfy followRestart.
    Get-ChildItem -Path $root -Filter ("superman_returns_{0}_r*.log" -f $env:SR_INSTANCE_ID) -ErrorAction SilentlyContinue |
      ForEach-Object { try { Remove-Item $_.FullName -Force } catch {} }
    Note ("gates: {0}" -f ($allGates -join ","))


    # iter-464 bgruns: no console window (Start-Process gave each roll a Windows
    # Terminal window that took the foreground); the game backgrounds its own window.
    if (Test-SrStuckHold) {   # Fable ruling 13: a hold that appeared during preparation or the retry sleep
      $failReason = "SR_STUCK_HOLD exists - refusing to launch"
      Note ("ABORTING ATTEMPT (no process touched): {0}" -f $failReason)
      $stepOk = $false
      throw $failReason
    }
    $proc = Start-SrGameProcess -FilePath $exePath -WorkingDirectory $root
    # Open the process handle NOW: a process object that never touched .Handle
    # can report ExitCode = $null after the exit (Windows PowerShell 5.1), and [int]$null
    # is 0 - an expectExitCode step would then read a crash as a clean exit.
    $null = $proc.Handle
    $script:WatchProc = $proc   # every Wait-For from here on also watches the game
    Note ("launched PID {0}" -f $proc.Id)
    # 2026-10-14 roll-stage: the game holds the stage lease too, so it outlives a driver killed outright
    if ($script:StageLeaseId) {
      $la = Invoke-SrStageLeaseAdd -Stage $Stage -LeaseId $script:StageLeaseId -HolderPid $proc.Id
      Note ("stage: {0}" -f $la.Detail)
    }
    # PID is persisted IMMEDIATELY. If HWND resolution then fails, the failed
    # run is still attributable to a process (director finding, iter-450Y).
    Update-SrProvenanceProcess -Roots $roots -ProcessId $proc.Id -Hwnd ""

    # ---- resolve + validate the HWND -------------------------------------
    $script:__h = [IntPtr]::Zero
    $got = Wait-For {
      $p = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
      if ($p -and $p.MainWindowHandle -ne 0) { $script:__h = $p.MainWindowHandle; return $true }
      return $false
    } 90000
    if (-not $got) { throw "no MainWindowHandle within 90s" }
    $hwnd = $script:__h
    $pidOut = 0
    [void][SRWin32]::GetWindowThreadProcessId($hwnd, [ref]$pidOut)
    if ($pidOut -ne $proc.Id) { throw "HWND $hwnd belongs to PID $pidOut, not $($proc.Id)" }
    if (-not [SRWin32]::IsWindowVisible($hwnd)) { throw "HWND $hwnd is not visible" }
    Note ("HWND {0} validated for PID {1}, visible" -f $hwnd, $proc.Id)
    Update-SrProvenanceProcess -Roots $roots -ProcessId $proc.Id -Hwnd ([string]$hwnd)

    # iter-464 bgruns: the harness keyboard channel of THIS process (PID-stamped;
    # the game creates it before its window, so this normally opens at once).
    if ($script:InputMode -eq 'harness') {
      $script:Harness = Open-SrHarnessInput $env:SR_INSTANCE_ID $proc.Id 30000
      if ($script:Harness.ok) {
        Note ("input: harness channel {0} open for PID {1}" -f $script:Harness.name, $proc.Id)
      } elseif ($script:HasKeySteps) {
        throw ("harness keyboard channel {0} not available for PID {1}: {2} (an exe older than iter-464 has no channel: use -ForegroundInput)" -f $script:Harness.name, $proc.Id, $script:Harness.reason)
      } else {
        Note ("input: harness channel {0} not available ({1}) - this scenario sends no keys, continuing" -f $script:Harness.name, $script:Harness.reason)
      }
    }

    # ---- steps ------------------------------------------------------------
    $runDeadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $stepOk = $true
    foreach ($s in $scn.steps) {
      $nm = $s.name
      # Fable ruling 13: no step (no input) runs once a stuck load is held.
      [void](Test-SrHoldLatch $proc)
      if ($script:HoldSeen) { $failReason = ("step '{0}': not run - SR_STUCK_HOLD exists (a stuck load was caught)" -f $nm); $stepOk = $false; break }
      # iter-467 forcing: a cutscene-skip step runs only while cutscene skipping is ON (Resolve-SrCutsceneSkip).
      if ($s.cutsceneSkip -and -not $script:SkipCut.on) {
        Note ("step '{0}': a cutscene-skip step - NOT run (cutscene skipping OFF: {1})" -f $nm, $script:SkipCut.why)
        continue
      }
      # 2026-10-14 chapter seeds: `onlyIfObserved` - the step runs only when the line the latest `expect` observed
      # matches (perf_run --from-save: A = METROPOLIS only after a load that landed on the carousel, not in the HUD).
      if (-not (Test-SrOnlyIfObserved $s $script:LastObserved)) {
        Note ("step '{0}': NOT run - the latest observation does not match /{1}/ -- {2}" -f $nm, $s.onlyIfObserved, $script:LastObserved)
        continue
      }
      # 2026-10-15: a later step's `expectSince` names this one - its boundary is armed here, before this step acts
      if ($script:ArmPlan.ContainsKey($nm)) {
        Pump-Log
        $script:Armed[$nm] = New-SrArmSnapshot $script:ArmPlan[$nm] { param($p) Count-Of $p }
        Note ("step '{0}': boundary armed - later steps count only lines from here ({1})" -f $nm,
              (($script:Armed[$nm].GetEnumerator() | ForEach-Object { "/{0}/ {1} earlier hit(s)" -f $_.Key, $_.Value }) -join '; '))
      }
      # iter-456q: the step's own entry instant. The sustained-black gate counts
      # ONLY frames written after this, because terminal black is not a unique
      # signature - the same black frame occurred nine times during boot in the
      # invalid run, so "we have seen black" is not "black started here".
      $stepEntered = Get-Date
      if ((Get-Date) -ge $runDeadline) { $failReason = "run timeout before step '$nm'"; $stepOk = $false; break }
      # iter-463 (femenu review finding 3): an `expectExitCode` step makes the process
      # exit the PASS condition. It waits up to exitTimeoutMs (default 10000) for the
      # process to end and passes only on exactly that exit code; a crash code, or a
      # process still running, fails the roll. It must be the LAST step: once it passes
      # the game is gone, so the scenario ends here and the pre-final liveness check
      # (which would call a gone game a failure) is skipped.
      if ($null -ne $s.expectExitCode) {
        $xto = $(if ($s.exitTimeoutMs) { [int]$s.exitTimeoutMs } else { 10000 })
        $want = [int]$s.expectExitCode
        Note ("step '{0}': EXPECT process exit with code {1} (<= {2} ms)" -f $nm, $want, $xto)
        $xdl = (Get-Date).AddMilliseconds($xto)
        while (-not $proc.HasExited -and (Get-Date) -lt $xdl) { Pump-Log; Start-Sleep -Milliseconds 100 }
        if (-not $proc.HasExited) {
          $failReason = "step '$nm': the process was still running $xto ms later (expected exit code $want) - THIS IS THE BLOCKER"
          $stepOk = $false; break
        }
        if ($null -eq $proc.ExitCode) {
          $failReason = "step '$nm': the process exited but its exit code is unreadable - THIS IS THE BLOCKER"
          $stepOk = $false; break
        }
        $code = [int]$proc.ExitCode
        if ($code -ne $want) {
          $failReason = ("step '{0}': process exited with code {1} (0x{2:X8}), expected {3} - THIS IS THE BLOCKER" -f $nm, $code, $code, $want)
          $stepOk = $false; break
        }
        Pump-Log
        Note ("step '{0}': observed -- process exited (code {1})" -f $nm, $code)
        $last = @($scn.steps)[-1]
        if (-not [object]::ReferenceEquals($s, $last)) {
          $failReason = "step '$nm': an expectExitCode step must be the last step (the game is gone)"
          $stepOk = $false; break
        }
        $script:ExitedAsExpected = $true
        Note ("step '{0}': DONE" -f $nm)
        break
      }
      # iter-465 player: `followRestart` - the MODS menu's RESTART NOW relaunches the exe
      # (kernel/mod_menu.cpp modMenuSpawnRestartIfRequested). This step waits for THIS process
      # to exit with code 0, finds the child by its own boot line ("[MODMENU] restarted by the
      # MODS menu (generation G, parent pid P, pid C)" in superman_returns_<id>_r<G>.log), and
      # from then on every step drives the CHILD: its PID, window, harness channel, log and frame
      # folder (<frames>_r<G>, the dump dir the parent hands down). Pattern counts restart at 0,
      # so later awaits see only the child's lines. The parent's log is kept as run.log, the
      # child's as run_r<G>.log; the teardown stops the child.
      if ($s.followRestart) {
        # timeoutMs bounds each of the two waits (the parent's exit, the child's boot line).
        $cto = $(if ($s.timeoutMs) { [int]$s.timeoutMs } else { 60000 })
        $xto = $cto
        $gen = $script:RestartGen + 1
        $childLog = Join-Path $root ("superman_returns_{0}_r{1}.log" -f $env:SR_INSTANCE_ID, $gen)
        Note ("step '{0}': FOLLOW RESTART - parent PID {1} must exit 0 (<= {2} ms), then the child logs to {3}" -f $nm, $proc.Id, $xto, $childLog)
        $xdl = (Get-Date).AddMilliseconds($xto)
        while (-not $proc.HasExited -and (Get-Date) -lt $xdl) { Pump-Log; Start-Sleep -Milliseconds 100 }
        if (-not $proc.HasExited) { $failReason = "step '$nm': the parent was still running $xto ms later - RESTART NOW did not exit it - THIS IS THE BLOCKER"; $stepOk = $false; break }
        if ($null -eq $proc.ExitCode -or [int]$proc.ExitCode -ne 0) {
          $failReason = ("step '{0}': the parent exited with code {1}, not 0 - THIS IS THE BLOCKER" -f $nm, $proc.ExitCode); $stepOk = $false; break
        }
        Pump-Log
        if (Test-Path $script:LogPath) { Copy-Item $script:LogPath $roots.RunLog -Force -ErrorAction SilentlyContinue }
        $parentPid = $proc.Id
        if ($script:Harness) { Close-SrHarnessInput $script:Harness; $script:Harness = $null }
        if ($script:Reader) { try { $script:Reader.Dispose() } catch {}; $script:Reader = $null }
        if ($script:Stream) { try { $script:Stream.Dispose() } catch {}; $script:Stream = $null }
        $script:LogPath = $childLog
        foreach ($k in @($script:Counts.Keys)) { $script:Counts[$k] = 0; $script:LastHit[$k] = "" }
        # the counts restart with the child's log: an armed boundary now means "every line of the child"
        foreach ($b in @($script:Armed.Values)) { foreach ($k in @($b.Keys)) { $b[$k] = 0 } }
        $restartPat = '\[MODMENU\] restarted by the MODS menu \(generation (\d+), parent pid (\d+), pid (\d+)\)'
        Register-Pattern $restartPat
        $script:WatchProc = $null   # the parent's exit is the point here: no game is watched until the child
        if (-not (Wait-For { (Count-Of $restartPat) -gt 0 } $cto)) {
          $failReason = "step '$nm': no child announced itself in $childLog within $cto ms - THIS IS THE BLOCKER"; $stepOk = $false; break
        }
        $null = $script:LastHit[$restartPat] -match $restartPat
        $childPid = [int]$Matches[3]
        if ([int]$Matches[2] -ne $parentPid) {
          $failReason = ("step '{0}': the child names parent pid {1}, not {2} (a stale log?) - THIS IS THE BLOCKER" -f $nm, $Matches[2], $parentPid); $stepOk = $false; break
        }
        $proc = Get-Process -Id $childPid -ErrorAction SilentlyContinue
        if (-not $proc) { $failReason = "step '$nm': child PID $childPid is already gone - THIS IS THE BLOCKER"; $stepOk = $false; break }
        $null = $proc.Handle
        $script:WatchProc = $proc
        # Fable ruling 13: adopt (and so later stop) the child only if that PID runs this exe - a reused PID is never ours.
        $cpath = $null; try { $cpath = $proc.Path } catch {}
        if (-not $cpath -or -not @($script:SweepPaths | Where-Object { [string]::Equals([IO.Path]::GetFullPath($cpath),
                                     [IO.Path]::GetFullPath($_), [StringComparison]::OrdinalIgnoreCase) }).Count) {
          $failReason = ("step '{0}': child PID {1} runs '{2}', not this exe - not adopted, never stopped - THIS IS THE BLOCKER" -f $nm, $childPid, $cpath)
          $proc = $null; $script:WatchProc = $null; $stepOk = $false; break
        }
        $script:__h = [IntPtr]::Zero
        $got = Wait-For {
          $p = Get-Process -Id $childPid -ErrorAction SilentlyContinue
          if ($p -and $p.MainWindowHandle -ne 0) { $script:__h = $p.MainWindowHandle; return $true }
          return $false
        } 90000
        if (-not $got) { $failReason = "step '$nm': child PID $childPid has no window within 90 s"; $stepOk = $false; break }
        $hwnd = $script:__h
        if ($script:InputMode -eq 'harness') {
          $script:Harness = Open-SrHarnessInput $env:SR_INSTANCE_ID $childPid 30000
          if (-not $script:Harness.ok -and $script:HasKeySteps) {
            $failReason = ("step '{0}': the child's harness channel {1} is not available: {2}" -f $nm, $script:Harness.name, $script:Harness.reason); $stepOk = $false; break
          }
        }
        $frameDir = $frameDir + ("_r{0}" -f $gen)
        $script:RestartGen = $gen
        Note ("step '{0}': now driving the child PID {1} (generation {2}), HWND {3}, frames {4}" -f $nm, $childPid, $gen, $hwnd, $frameDir)
        if ($script:StageLeaseId) {       # the relaunched game runs from the stage too
          $la = Invoke-SrStageLeaseAdd -Stage $Stage -LeaseId $script:StageLeaseId -HolderPid $childPid
          Note ("stage: {0}" -f $la.Detail)
        }
        Note ("step '{0}': DONE" -f $nm)
        continue
      }
      # exit-first (the Get-SrAliveReason rule): an exiting process kills its HWND in the
      # same instant, so checking the window first reported 'window vanished' and lost
      # the exit code (iter-463 femenu loadexit3, a clean SR_Quit exit).
      if ($proc.HasExited) { $failReason = "process exited (code $($proc.ExitCode)) before step '$nm'"; $stepOk = $false; break }
      if (-not [SRWin32]::IsWindow($hwnd)) { $failReason = "window vanished before step '$nm'"; $stepOk = $false; break }
      $script:WaitExit = $null   # the game is alive at this step's start: no earlier wait's exit applies to it

      # -- iter-455l r2: the hardened trio is owned by the ONE shared body --
      # (Invoke-HardenedDialog in sr_drive_core.ps1 - the same code the test
      # drives). The await + settle steps are consumed by the body at the
      # clear step, so the legacy handlers must not run them a second time.
      if ($script:HardenedPlan -and ($nm -ceq $script:HardenedPlan.awaitName -or $nm -ceq $script:HardenedPlan.settleName)) {
        Note ("step '{0}': owned by the hardened-dialog body (runs at '{1}')" -f $nm, $script:HardenedPlan.clearName)
        continue
      }
      if ($script:HardenedPlan -and $nm -ceq $script:HardenedPlan.clearName) {
        $kd = $KEYS[$s.key.ToUpper()]
        if (-not $kd) { $failReason = "step '$nm': unknown key '$($s.key)'"; $stepOk = $false; break }
        $confirmPat = $s.confirm
        if (-not $confirmPat -and $kd.mask -gt 0) { $confirmPat = ("KBPAD_OBS.*mappedMask=0x{0:X4}" -f $kd.mask) }
        $awaitStep  = @($scn.steps)[$script:HardenedPlan.awaitIdx]
        $settleStep = @($scn.steps)[$script:HardenedPlan.settleIdx]
        Note ("step '{0}': HARDENED body - await '{1}' -> settle '{2}' -> single-deadline engine" -f $nm, $script:HardenedPlan.awaitName, $script:HardenedPlan.settleName)
        $script:DriveEpoch = Get-Date
        $script:HardHeld = @{ vk = $kd.vk; ext = [bool]$kd.ext }
        $probes = @{
          NowMs   = { [int64]((Get-Date) - $script:DriveEpoch).TotalMilliseconds }
          CountOf = { param($pat) Pump-Log; Count-Of $pat }
          HitOf   = { param($pat) $script:LastHit[$pat] }
          Alive   = { # exit-first (Get-SrAliveReason): an exiting process
                      # kills its HWND in the same instant, and 'window
                      # vanished' would mask the real cause (director, r3).
                      $he = $proc.HasExited
                      Get-SrAliveReason $he $(if ($he) { $proc.ExitCode } else { 0 }) ([SRWin32]::IsWindow($hwnd)) $hwnd }
          Focus   = { Focus-Game $hwnd }
          KeyDown = { [void]$script:HeldVks.Add($script:HardHeld); Send-Key $script:HardHeld.vk $false $script:HardHeld.ext }
          # iter-456q: the release is ESTABLISHED before the key leaves the
          # held list. Removing it first meant Release-AllKeys could no longer
          # rescue a key whose up-event never landed - the failure mode the
          # driver's own rules warn about, in the one path that holds a key
          # longest.
          KeyUp   = { [void](Send-Key $script:HardHeld.vk $true $script:HardHeld.ext)
                      if (Assert-KeyReleased $script:HardHeld.vk $script:HardHeld.ext) {
                        $script:HeldVks.Remove($script:HardHeld)
                      } }
          Sleep   = { param($ms)
                      $dl = (Get-Date).AddMilliseconds($ms)
                      while ((Get-Date) -lt $dl) { Pump-Log; Start-Sleep -Milliseconds 100 } }
          Note    = { param($m) Note $m }
        }
        $res = Invoke-HardenedDialog $awaitStep $settleStep $s $confirmPat $probes
        Note ("step '{0}': hardened {1} (phase={2} fatal={3}) - pulses={4} delivered={5} elapsed={6} ms" -f $nm, $res.reason, $res.phase, $res.fatal, $res.pulses, $res.delivered, $res.elapsedMs)
        if (-not $res.ok) {
          $failReason = "step '$nm': " + $res.reason + $(if ($res.fatal) { " (FATAL probe, phase $($res.phase))" } else { " - THIS IS THE BLOCKER" })
          $stepOk = $false; break
        }
        Note ("step '{0}': observed -- {1}" -f $nm, $script:LastHit[$s.expect])
        $script:LastObserved = [string]$script:LastHit[$s.expect]
        Note ("step '{0}': DONE" -f $nm)
        continue
      }

      $to = $(if ($s.timeoutMs) { [int]$s.timeoutMs } else { 90000 })

      # iter-463: `expectFresh` - the step's expect counts only lines that
      # appear AFTER the step starts. Needed when the marker already appeared
      # earlier in the run (a screen path requested at the title and again in
      # game, e.g. screens/misc/ScrGenPop), where a whole-run count would pass
      # before the action was even sent.
      $expBase = 0
      if ($s.expect -and $s.expectFresh) {
        Pump-Log
        $expBase = Count-Of $s.expect
        Note ("step '{0}': expectFresh - {1} earlier hit(s) of the expect are ignored" -f $nm, $expBase)
      }
      # 2026-10-15: `expectSince` - only lines after the named step's boundary (armed at its entry) count
      if ($s.expect -and $s.expectSince) {
        $sb = Get-SrSinceBase $s $script:Armed
        if (-not $sb.ok) { $failReason = "step '$nm': " + $sb.reason; $stepOk = $false; break }
        $expBase = $sb.base
        Note ("step '{0}': expectSince '{1}' - {2} hit(s) of the expect before that step are ignored" -f $nm, $s.expectSince, $expBase)
      }

      # -- PRECONDITION ----------------------------------------------------
      if ($s.await) {
        Note ("step '{0}': PRECONDITION /{1}/ (<= {2} ms)" -f $nm, $s.await, $to)
        if (-not (Wait-For { (Count-Of $s.await) -gt 0 } $to)) {
          $failReason = "step '$nm': precondition /$($s.await)/ never appeared$(Get-WaitExitNote)"; $stepOk = $false; break
        }
        Note ("step '{0}': precondition met -- {1}" -f $nm, $script:LastHit[$s.await])
      }

      # -- ACTION (+ delivery confirmation, optionally repeated) -----------
      if ($s.key) {
        $kd = $KEYS[$s.key.ToUpper()]
        if (-not $kd) { $failReason = "step '$nm': unknown key '$($s.key)'"; $stepOk = $false; break }
        $confirmPat = $s.confirm
        if (-not $confirmPat -and $kd.mask -gt 0) { $confirmPat = ("KBPAD_OBS.*mappedMask=0x{0:X4}" -f $kd.mask) }
        $hold      = $(if ($s.holdMs)    { [int]$s.holdMs }    else { 250 })
        $maxPulse  = $(if ($s.maxPulses) { [int]$s.maxPulses } else { 1 })
        $gap       = $(if ($s.gapMs)     { [int]$s.gapMs }     else { 1500 })
        $confirmMs = $(if ($s.confirmMs) { [int]$s.confirmMs } else { 6000 })
        $expectMs  = $(if ($s.expectMs)  { [int]$s.expectMs }  else { $to })
        $delivered = 0

        for ($pulse = 1; $pulse -le $maxPulse; $pulse++) {
          [void](Test-SrHoldLatch $proc)   # Fable ruling 13: no key pulse once a stuck load is held
          if ($script:HoldSeen) { $failReason = ("step '{0}': stopped before pulse {1} - SR_STUCK_HOLD (a stuck load was caught)" -f $nm, $pulse); $stepOk = $false; break }
          if (-not [SRWin32]::IsWindow($hwnd)) { $failReason = "step '$nm': window vanished mid-pulse"; $stepOk = $false; break }
          # Focus is re-asserted before EVERY pulse. iter-450UK caught a press
          # silently discarded because the window had lost focus between two.
          # A focus failure on an `optional` step must NOT abort the scenario:
          # iter-450W lost a whole run because this path set failReason directly
          # and bypassed the optional check further down.
          if (-not (Focus-Game $hwnd)) {
            $msg = "step '$nm': could not foreground HWND $hwnd (operator likely holds the desktop)"
            if ($s.optional) {
              Note ("OPTIONAL step could not take focus, skipping -- {0}" -f $msg)
              Add-Content -Path ($softFailuresTxt) -Value $msg -Encoding ascii
              break
            }
            $failReason = $msg; $stepOk = $false; break
          }
          Pump-Log
          $mark = $(if ($confirmPat) { Count-Of $confirmPat } else { 0 })
          # holdUntilConfirmed: keep the key DOWN until the guest is observed
          # sampling it, up to holdMs. A fixed short press is a coin flip
          # against a slow poll - iter-450V measured the front-end polling
          # XamInputGetState at 0.05 Hz, where a 180 ms press has ~1% odds of
          # being seen. A human faced with an unresponsive menu holds the
          # button; this does the same, and releases the moment it lands.
          $holdConfirmed = $false
          $huc = [bool]$s.holdUntilConfirmed
          Note ("step '{0}': pulse {1}/{2} {3} for {4}{5} ms" -f $nm, $pulse, $maxPulse, $s.key, $(if ($huc) { "up to " } else { "" }), $hold)
          $held = @{ vk = $kd.vk; ext = [bool]$kd.ext }
          try {
            [void]$script:HeldVks.Add($held)
            Send-Key $kd.vk $false $held.ext
            if ($huc -and $confirmPat) {
              $hdl = (Get-Date).AddMilliseconds($hold)
              while ((Get-Date) -lt $hdl) {
                Pump-Log
                if ((Count-Of $confirmPat) -gt $mark) { $holdConfirmed = $true; break }
                Start-Sleep -Milliseconds 100
              }
            } else {
              Start-Sleep -Milliseconds $hold
            }
          } finally {
            # iter-456q: key-up ALWAYS, and now PROVEN. The result of Send-Key
            # was discarded and $held was removed unconditionally, so a release
            # that never landed left the driver believing nothing was held -
            # exactly the state in which Release-AllKeys has nothing to rescue.
            [void](Send-Key $kd.vk $true $held.ext)
            if (Assert-KeyReleased $kd.vk $held.ext) {
              $script:HeldVks.Remove($held)
            } else {
              Note ("step '{0}': key vk=0x{1:X2} could NOT be proven released - left on the held list for the unwind" -f $nm, $kd.vk)
            }
          }
          if ($confirmPat) {
            if ($holdConfirmed -or (Wait-For { (Count-Of $confirmPat) -gt $mark } $confirmMs)) {
              $delivered++
              Note ("step '{0}': pulse {1} DELIVERED -- {2}" -f $nm, $pulse, $script:LastHit[$confirmPat])
            } else {
              Note ("step '{0}': pulse {1} NOT DELIVERED (no /{2}/) - the guest never saw it" -f $nm, $pulse, $confirmPat)
            }
          } else { $delivered++ }

          # Repeat only while the transition we are driving toward is absent.
          if ($s.expect) {
            $w = $(if ($pulse -lt $maxPulse) { $gap } else { $expectMs })
            if (Wait-For { (Count-Of $s.expect) -gt $expBase } $w) { break }
          } elseif ($pulse -lt $maxPulse) { Start-Sleep -Milliseconds $gap }
          # a wait that ended on the game's exit ends the pulses: a dead game is not "a pulse not delivered"
          if ($script:WaitExit) { $failReason = "step '$nm': the game $($script:WaitExit) during pulse $pulse"; $stepOk = $false; break }
        }
        if (-not $stepOk) { break }
        if ($confirmPat -and $delivered -eq 0) {
          # `optional` exists because a discovery run must not lose its
          # observation window to one missed press. iter-450V threw away a
          # 4-minute measurement when an undelivered DOWN aborted the run
          # before the step that was the whole point.
          $msg = "step '$nm': NO pulse reached the guest across $maxPulse attempt(s)"
          if ($s.optional) {
            Note ("OPTIONAL step failed, continuing -- {0}" -f $msg)
            Add-Content -Path ($softFailuresTxt) -Value $msg -Encoding ascii
          } else { $failReason = $msg; $stepOk = $false; break }
        }
        Note ("step '{0}': {1} of {2} pulse(s) delivered to the guest" -f $nm, $delivered, $maxPulse)
      }

      # -- iter-464: MOUSE ACTION (docs/MOUSE_CURSOR.md 8) ------------------
      if ($s.mouse) {
        if (-not $script:MouseFeed -and -not (Focus-Game $hwnd)) {
          $msg = "step '$nm': could not foreground HWND $hwnd for the mouse"
          if ($s.optional) { Note ("OPTIONAL step could not take focus, skipping -- {0}" -f $msg); Add-Content -Path ($softFailuresTxt) -Value $msg -Encoding ascii }
          else { $failReason = $msg; $stepOk = $false; break }
        } else {
          $mr = Invoke-MouseScript $hwnd ([string]$s.mouse) $nm
          if (-not $mr.ok) { $failReason = "step '$nm': mouse: $($mr.reason)"; $stepOk = $false; break }
        }
      }

      # -- EXPECTED OBSERVATION --------------------------------------------
      if ($s.expect -and -not $s.singleDeadline) {
        $eto = $(if ($s.expectMs) { [int]$s.expectMs } else { $to })
        Note ("step '{0}': EXPECT /{1}/ (<= {2} ms)" -f $nm, $s.expect, $eto)
        if (-not (Wait-For { (Count-Of $s.expect) -gt $expBase } $eto)) {
          $msg = "step '$nm': expected /$($s.expect)/ never appeared$(Get-WaitExitNote) - THIS IS THE BLOCKER"
          if ($s.optional) {
            Note ("OPTIONAL step failed, continuing -- {0}" -f $msg)
            Add-Content -Path ($softFailuresTxt) -Value $msg -Encoding ascii
          } else { $failReason = $msg; $stepOk = $false; break }
        } else {
          Note ("step '{0}': observed -- {1}" -f $nm, $script:LastHit[$s.expect])
          $script:LastObserved = [string]$script:LastHit[$s.expect]
        }
      }

      # iter-456q: THE GATE RUNS BEFORE THE DWELL. A step that declares
      # requireBlackMs will not spend its measurement window unless the era
      # under test has demonstrably been entered since this step started.
      if ($s.requireBlackMs) {
        $budget = if ($s.blackTimeoutMs) { [int]$s.blackTimeoutMs } else { 120000 }
        Note ("step '{0}': BLACK GATE - require {1} ms sustained post-entry black within {2} ms" -f $nm, [int]$s.requireBlackMs, $budget)
        if (-not (Wait-SustainedBlack $frameDir $stepEntered ([int]$s.requireBlackMs) $budget)) {
          $stepOk = $false
          $failReason = "step '$nm': the sustained-black gate was not met - the era under test was not entered, so the measurement dwell was NOT spent"
          Note $failReason
          break
        }
      }
      # iter-458 (Astra 038): a step that declares requireVisibleMs does not
      # finish - so the NEXT step's input is not sent - until post-entry frames
      # have been non-black for that long. Use it as a gate-only step (no key)
      # in front of the step that presses.
      if ($s.requireVisibleMs) {
        $budget = if ($s.visibleTimeoutMs) { [int]$s.visibleTimeoutMs } else { 180000 }
        Note ("step '{0}': VISIBLE GATE - require {1} ms sustained post-entry non-black within {2} ms" -f $nm, [int]$s.requireVisibleMs, $budget)
        if (-not (Wait-SustainedVisible $frameDir $stepEntered ([int]$s.requireVisibleMs) $budget)) {
          $stepOk = $false
          $failReason = "step '$nm': the sustained-visible gate was not met - no visible screen, so no input was sent"
          Note $failReason
          break
        }
      }

      # Post-action dwell exists so an era can compose and be captured; it is
      # never load-bearing for a verdict.
      # iter-458: the game must SURVIVE the dwell (Invoke-DwellBody); the old
      # loop broke on a lost window and still reported the step DONE.
      if ($s.dwellMs) {
        Note ("step '{0}': dwell {1} ms (capture window)" -f $nm, [int]$s.dwellMs)
        $srDwellAlive = (New-LivenessProbe $proc $hwnd { param($h) [SRWin32]::IsWindow($h) })
        $dw = Invoke-DwellBody $nm ([int]$s.dwellMs) 250 @{
          NowMs = { [int64]([DateTime]::UtcNow - [DateTime]'1970-01-01').TotalMilliseconds }
          # Fable ruling 13: a hold ends the dwell like a lost game.
          Alive = { [void](Test-SrHoldLatch $proc)
                    if ($script:HoldSeen) { "is held: SR_STUCK_HOLD exists (a stuck load was caught)" } else { & $srDwellAlive } }
          Pump  = { Pump-Log }
          Sleep = { param($ms) Start-Sleep -Milliseconds $ms }
          Note  = { param($m) Note $m }
        }
        if (-not $dw.ok) { $failReason = $dw.reason; $stepOk = $false; break }
      }
      Note ("step '{0}': DONE" -f $nm)
    }

    # iter-456q: NO VERDICT LINE HERE. It is emitted by Invoke-CleanupAndFinalize
    # in the finally block, AFTER cleanup and the health adjudication - because
    # a run that announces success and then fails has already told
    # the operator the wrong thing.
    # iter-458: all steps passing is not a success if the game is already gone.
    if ($script:ExitedAsExpected) {
      # The last step required the exit and saw the right code (see expectExitCode).
      $pre = @{ ok = $stepOk; failReason = $failReason }
    } else {
    $pre = Invoke-PreFinalLiveness $stepOk $failReason `
             (New-LivenessProbe $proc $hwnd { param($h) [SRWin32]::IsWindow($h) })
    }
    if (-not $pre.ok -and $stepOk) { Note $pre.failReason }
    $failReason = $pre.failReason
    $stepsOkForFinal = $pre.ok
  }
  catch {
    if (-not $failReason) { $failReason = $_.Exception.Message }
    Note ("EXCEPTION: {0}" -f $failReason)
  }
  finally {
    # Fable ruling 13: latch the hold FIRST, so no cleanup failure below can cost a held game its stage lease.
    [void](Test-SrHoldLatch $proc)
    # iter-456q: ONE finalization, in ONE order: release -> read counters ->
    # adjudicate -> ANNOUNCE. Nothing announces a verdict before everything that
    # could change it has run.
    if ($null -eq $stepsOkForFinal) { $stepsOkForFinal = $false }
    # COMPOSED: cleanup runs INSIDE the finalizer through the production probes,
    # mutating the ONE health record, which is then read once. No pre-capture,
    # no hardcoded zero, no second tally.
    $fin = Invoke-CleanupAndFinalize ([bool]$stepsOkForFinal) ([string]$failReason) `
             $script:HeldVks 5 $script:InputHealth (New-ProductionKeyProbes)
    $script:HeldVks.Clear()
    foreach ($k in @($fin.remaining)) { [void]$script:HeldVks.Add($k) }
    $ok = $fin.ok
    $failReason = $fin.failReason
    # iter-464 bgruns: the channel is closed only AFTER the release adjudication
    # above (which reads it back); the game's own merge count is the receipt.
    if ($script:Harness) {
      $hs = Get-SrHarnessStats $script:Harness
      if ($hs) { Note ("harness channel: the game merged it {0} time(s), last mask 0x{1:X4}" -f $hs.polls, $hs.lastMask) }
      Close-SrHarnessInput $script:Harness
      $script:Harness = $null
    }
    Pump-Log
    Note ("log lines consumed: {0}" -f $script:LinesSeen)
    foreach ($p in @($script:Counts.Keys | Sort-Object)) {
      Note ("  pattern /{0}/ -> {1} hit(s)" -f $p, $script:Counts[$p])
    }
    # step 4 (Fable ruling 13): a held stuck load keeps this attempt's own game alive for the probe - nothing is killed.
    Set-SrHoldVerdict $proc
    if ($script:Reader) { try { $script:Reader.Dispose() } catch {}; $script:Reader = $null }
    if ($script:Stream) { try { $script:Stream.Dispose() } catch {}; $script:Stream = $null }
    if (-not $KeepAlive -and -not $script:HeldAlive -and $proc -and -not $proc.HasExited) {
      if ($script:GracefulQuit) {
        # tools/graceful-quit (TOOLKIT 1.8m): ask the game to quit; it drains, prints its terminal snapshot and
        # exits. Every failure falls back to the Stop-Process below inside Invoke-SrGracefulQuit. The roll's verdict
        # is unchanged by the quit; graders read the [QUIT] lines of the log copied below.
        # The receipt (quit.json) is written whatever happens: the graders count a snapshot complete only with it.
        # Fable ruling 13: the quit's own kill fallback asks the hold latch first (a hold can arrive during the quit;
        # once the request is sent the game quits itself, and the hold still stops the run below).
        $gq = $null
        try {
          $qp = New-SrQuitProbes $proc
          $script:QuitKillProbe = $qp.Kill
          $script:QuitSignalProbe = $qp.Signal
          $script:QuitProc = $proc
          $qp.Signal = { param($ev) if (-not (Test-SrHoldLatch $script:QuitProc)) { & $script:QuitSignalProbe $ev } }
          $qp.Kill = { if (-not (Test-SrHoldLatch $script:QuitProc)) { & $script:QuitKillProbe } }
          $gq = Invoke-SrGracefulQuit -Instance $env:SR_INSTANCE_ID -ProcessId $proc.Id -AckTimeoutMs $script:QuitAckMs `
                  -ExitTimeoutMs $script:QuitExitMs -P $qp
          # native/quit-drain: the game's stranded record (its complete line's stranded=N, its [QUIT] stranded lines)
          Add-SrQuitTerminalInfo $gq $script:LogPath
        } catch {
          $gq = @{ result = 'harness-error'; graceful = $false; fellBack = $true; exitCode = $null; ackMs = $null
                   exitMs = $null; request = ''; ack = ''; detail = ('sr_drive: {0} - Stop-Process' -f $_.Exception.Message) }
          if (-not (Test-SrHoldLatch $proc)) { try { Stop-Process -Id $proc.Id -Force -ErrorAction Stop } catch {} }
        } finally {
          if (-not (Write-SrQuitRecord (Join-Path $roots.AttemptDir "quit.json") $gq)) {
            Note "WARN: quit: the receipt quit.json could not be written - the graders will not count the snapshot"
          }
        }
        Note (Format-SrQuitResult $gq)
      } else {
        if (-not (Test-SrHoldLatch $proc)) { try { Stop-Process -Id $proc.Id -Force -ErrorAction Stop } catch {} }
      }
      # Fable ruling 13: the next attempt's sweep refuses any copy of this exe, so our own child must be gone first.
      if (-not $script:HeldAlive) {
        try {
          if (-not $proc.WaitForExit(15000)) { Note ("WARN: our own game PID {0} still runs 15 s after its stop - the next sweep refuses it" -f $proc.Id) }
        } catch {}
        Start-Sleep -Milliseconds 800
      }
    }
    Set-SrHoldVerdict $proc   # a hold that arrived during the teardown
    # Per-attempt, and Set- not Add-: the old shared failure_reason.txt APPENDED,
    # so one file accumulated reasons from unrelated invocations.
    if ($failReason) {
      Set-Content -Path $roots.FailureTxt -Value ("attempt {0}: {1}" -f $attempt, $failReason) -Encoding ascii
    }
    # Artifacts retained for BOTH outcomes, inside this attempt's own root.
    # iter-465: after a followRestart the live log is the child's (run_r<G>.log); the
    # parent's run.log was copied when it exited.
    $finalLogCopy = $(if ($script:RestartGen -gt 0) { Join-Path $roots.AttemptDir ("run_r{0}.log" -f $script:RestartGen) } else { $roots.RunLog })
    if (Test-Path $script:LogPath) {
      Copy-Item $script:LogPath $finalLogCopy -Force -ErrorAction SilentlyContinue
    }
    # Outcome + a SORTED frame inventory, so coverage is answerable from the
    # manifest alone rather than inferred from filenames.
    $outcome = Resolve-RunOutcome @{ ok = $ok; failReason = $failReason }
    [void](Write-SrOutcome -Roots $roots -Ok $outcome.ok -FailReason ([string]$outcome.failReason) `
             -LogLines $script:LinesSeen)
  }
  if ((Test-SrStuckHold) -or $script:Leftover) { Note "no retry: a held stuck load or a leftover game stops this run (Fable ruling 13)"; break }
  if (-not $ok -and $attempt -le $Retries) { Note "retrying SEQUENTIALLY (K=1 rule)"; Start-Sleep -Seconds 3 }
}

$final = Resolve-RunOutcome @{ ok = $ok; failReason = $failReason }
# Fable ruling 13: 6 = a stuck load is held (it wins); 5 = a copy of this exe was already running (refused, nothing killed).
if (Test-SrStuckHold) { $final.exitCode = 6 } elseif ($script:Leftover) { $final.exitCode = 5 }
foreach ($n in @($script:InputEnvSaved.Keys)) {
  [Environment]::SetEnvironmentVariable($n, $script:InputEnvSaved[$n])   # $null removes it
}
Note ("RESULT: {0} after {1} attempt(s)" -f $final.result, $attempt)
} finally {
  # 2026-10-14 roll-stage: the stage lease ends with this driver; a game left running (-KeepAlive, or held by
  # SR_STUCK_HOLD) keeps it.
  if ($script:StageLeaseId) {
    [void](Test-SrHoldLatch $proc)
    $handoff = $(if (($KeepAlive -or $script:HeldAlive) -and $proc -and -not $proc.HasExited) { $proc.Id } else { 0 })
    $rel = Invoke-SrStageRelease -Stage $Stage -LeaseId $script:StageLeaseId -HandoffPid $handoff
    Note ("stage: {0}" -f $rel.Detail)
  }
  # Fable ruling 13: a held stuck load exits 6 on every path, an exception unwinding through here included.
  if (Test-SrStuckHold) { exit 6 }
}
exit $final.exitCode
