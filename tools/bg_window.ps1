# bg_window.ps1 - iter-464 bgruns: launch a GUI process (Xenia) WITHOUT taking the
# foreground, park its windows at the bottom of the z-order, and watch who owns
# the foreground. The user works on this PC while automated runs go; no harness
# may bring a window to the front (docs/TOOLKIT.md "Automated runs stay in the
# background"; our own game does this itself - kernel/host_automation.hpp).
#
# Dot-source for the functions:
#   . tools\bg_window.ps1
#   $p = Start-SrBackgroundProcess -FilePath $exe -ArgumentList '"<iso>"' -WorkingDirectory $dir
#   [void](Move-SrWindowToBottom -ProcessId $p.Id -TimeoutMs 30000)
# or run it:
#   powershell -File tools\bg_window.ps1 -BgLaunch <exe> [-BgCwd <dir>] [<arg> ...]
#     (each trailing <arg> is quoted for the child's command line, so a path with
#      spaces stays ONE argument: -BgLaunch <xenia_canary.exe> "<dir with spaces>\x.iso".
#      -BgArgs '<raw command-line tail>' is passed verbatim - safe in-process only: through
#      powershell.exe -File, Windows PowerShell 5.1 strips the inner quotes of an argument)
#   powershell -File tools\bg_window.ps1 -BgGuard <pid> [-BgGuardLog <log>]
#     (-BgGuard: for the LIFE of <pid>, whenever it takes the foreground without a user
#      gesture, hand the foreground back to the window that had it and push <pid>'s
#      windows to the bottom. -BgLaunch starts one by default; -BgNoGuard skips it)
#   powershell -File tools\bg_window.ps1 -BgWatch <out.log> -BgSeconds 300 [-BgFlag SupermanReturns,xenia_canary]
#     (-BgWatch: every 0.5 s log the foreground window's process; exit 1 if a
#      -BgFlag process ever held the foreground - the verification probe for this rule)
# The parameters carry a Bg prefix because dot-sourcing binds them into the
# caller's scope; a plain $Seconds or $Flag would clobber the caller's own.
#
# How: CreateProcess with STARTUPINFO wShowWindow = SW_SHOWNOACTIVATE, so the
# first ShowWindow of the child shows without activation; then every visible
# top-level window of the PID is moved to HWND_BOTTOM (SWP_NOACTIVATE).
# ASCII only.
[CmdletBinding(PositionalBinding = $false)]   # unnamed arguments go to -BgRest only
param(
    [string] $BgLaunch = "",
    [string] $BgArgs = "",
    [string] $BgCwd = "",
    [string] $BgWatch = "",
    [int]    $BgSeconds = 300,
    [string] $BgFlag = "SupermanReturns,xenia_canary",
    [int]    $BgGuard = 0,
    [string] $BgGuardLog = "",
    [long]   $BgPrevForeground = 0,
    [switch] $BgNoGuard,
    [Parameter(ValueFromRemainingArguments = $true)] [string[]] $BgRest = @()
)

# Lane sdl3-audio (RELEASE_PLAN step 3): every launch carries the SDL2 AND the SDL3 spelling of the audio
# driver / disk-file variables, so an SDL3 exe launched by an SDL2-era tool still goes to the disk driver.
. (Join-Path $PSScriptRoot 'sdl_audio_env.ps1')

if (-not ("SRBgWin" -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class SRBgWin {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct STARTUPINFO {
    public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
  }
  [StructLayout(LayoutKind.Sequential)]
  public struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  public static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit,
      uint flags, IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  public const int STARTF_USESHOWWINDOW = 1;
  public const short SW_SHOWNOACTIVATE = 4;
  public const uint SWP_NOSIZE = 0x1, SWP_NOMOVE = 0x2, SWP_NOACTIVATE = 0x10, SWP_NOOWNERZORDER = 0x200;
  public static readonly IntPtr HWND_BOTTOM = new IntPtr(1);

  // Starts cmdLine (the full command line, exe quoted) with SW_SHOWNOACTIVATE. Returns the PID.
  public static int StartNoActivate(string cmdLine, string cwd) {
    STARTUPINFO si = new STARTUPINFO();
    si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(null, new StringBuilder(cmdLine), IntPtr.Zero, IntPtr.Zero, false, 0,
                        IntPtr.Zero, string.IsNullOrEmpty(cwd) ? null : cwd, ref si, out pi))
      throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return pi.dwProcessId;
  }
  // Starts cmdLine with NO console window and NO inherited handles (CREATE_NO_WINDOW,
  // bInheritHandles = FALSE). For the detached guard: a child that inherited the caller's
  // stdout pipe would keep a capturing caller (`$x = & powershell ...`) waiting for as long as
  // the guarded app lives (measured: the v2_xenia4 test hung exactly so). Returns the PID.
  public static int StartHidden(string cmdLine, string cwd) {
    STARTUPINFO si = new STARTUPINFO();
    si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(null, new StringBuilder(cmdLine), IntPtr.Zero, IntPtr.Zero, false, 0x08000000,
                        IntPtr.Zero, string.IsNullOrEmpty(cwd) ? null : cwd, ref si, out pi))
      throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return pi.dwProcessId;
  }
  // Visible, unowned top-level windows of a process.
  public static List<IntPtr> TopWindowsOf(uint pid) {
    List<IntPtr> found = new List<IntPtr>();
    EnumWindows(delegate (IntPtr h, IntPtr l) {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h) && GetWindow(h, 4 /*GW_OWNER*/) == IntPtr.Zero) found.Add(h);
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static int ToBottom(uint pid) {
    int n = 0;
    foreach (IntPtr h in TopWindowsOf(pid)) {
      if (SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) n++;
    }
    return n;
  }
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
  // A user gesture that can legitimately activate a window: a mouse button, Alt (Alt+Tab)
  // or a Windows key held right now.
  public static bool GestureDown() {
    int[] vks = { 0x01, 0x02, 0x04, 0x12, 0x5B, 0x5C };
    foreach (int vk in vks) { if ((GetAsyncKeyState(vk) & 0x8000) != 0) return true; }
    return false;
  }
  // Hands the foreground back to `prev` when a launched child took it: attaching to
  // the current foreground thread lifts the foreground lock for this one call.
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool altTab);
  // Hands the foreground back to `prev` after a launched child took it. Attaching this thread's
  // input to the CURRENT foreground thread (and to prev's thread) lifts the foreground lock for
  // the call; a window that is still mid-activation can override one attempt, so it retries,
  // ending with SwitchToThisWindow. Synthesises no input.
  public static bool GiveBack(IntPtr prev) {
    if (prev == IntPtr.Zero || !IsWindow(prev)) return false;
    uint me = GetCurrentThreadId();
    for (int attempt = 0; attempt < 6; attempt++) {
      IntPtr fg = GetForegroundWindow();
      if (fg == prev) return true;
      uint fgPid, prevPid;
      uint fgThread = fg == IntPtr.Zero ? 0 : GetWindowThreadProcessId(fg, out fgPid);
      uint prevThread = GetWindowThreadProcessId(prev, out prevPid);
      bool a1 = fgThread != 0 && fgThread != me && AttachThreadInput(me, fgThread, true);
      bool a2 = prevThread != 0 && prevThread != me && prevThread != fgThread && AttachThreadInput(me, prevThread, true);
      try {
        BringWindowToTop(prev);
        SetForegroundWindow(prev);
        if (attempt >= 3 && GetForegroundWindow() != prev) SwitchToThisWindow(prev, true);
      } finally {
        if (a2) AttachThreadInput(me, prevThread, false);
        if (a1) AttachThreadInput(me, fgThread, false);
      }
      System.Threading.Thread.Sleep(15);
    }
    return GetForegroundWindow() == prev;
  }
  public static uint ForegroundPid() {
    IntPtr h = GetForegroundWindow();
    if (h == IntPtr.Zero) return 0;
    uint p; GetWindowThreadProcessId(h, out p); return p;
  }
}
"@
}

# One argument quoted for a Windows command line (CommandLineToArgvW rules): wrapped in
# quotes when it is empty or holds whitespace or a quote; backslashes before a quote doubled.
function ConvertTo-SrCommandLineArg([string]$Arg) {
    if ($Arg -and $Arg -notmatch '[\s"]') { return $Arg }
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append('"')
    $bs = 0
    foreach ($c in $Arg.ToCharArray()) {
        if ($c -eq [char]'\') { $bs++; continue }
        if ($c -eq [char]'"') { [void]$sb.Append([char]'\', 2 * $bs + 1); [void]$sb.Append('"'); $bs = 0; continue }
        if ($bs) { [void]$sb.Append([char]'\', $bs); $bs = 0 }
        [void]$sb.Append($c)
    }
    if ($bs) { [void]$sb.Append([char]'\', 2 * $bs) }
    [void]$sb.Append('"')
    return $sb.ToString()
}

# Launches without activation; returns the System.Diagnostics.Process (handle
# opened at once so ExitCode stays readable after the exit). The window that
# owned the foreground at launch rides along as .SrPrevForeground for the guard.
function Start-SrBackgroundProcess([string]$FilePath, [string]$ArgumentList = "", [string]$WorkingDirectory = "") {
    $exe = (Resolve-Path -LiteralPath $FilePath).Path
    $cmd = '"' + $exe + '"'
    if ($ArgumentList) { $cmd += ' ' + $ArgumentList }
    $cwd = if ($WorkingDirectory) { (Resolve-Path -LiteralPath $WorkingDirectory).Path } else { Split-Path -Parent $exe }
    Add-SrSdlAudioEnvMirror   # CreateProcess inherits this session's env: SDL2 + SDL3 audio env names
    $prev = [SRBgWin]::GetForegroundWindow()
    $procId = [SRBgWin]::StartNoActivate($cmd, $cwd)
    $p = Get-Process -Id $procId
    $null = $p.Handle
    $p | Add-Member -NotePropertyName SrPrevForeground -NotePropertyValue $prev -Force
    return $p
}

# Waits (up to $TimeoutMs) for the process's first visible top-level window and
# moves its windows to the bottom of the z-order without activating. Then GUARDS
# for $GuardMs: an app that activates itself anyway (a second ShowWindow, a
# SetForegroundWindow of its own) is pushed down again and the foreground is
# handed back to $PreviousForeground. Returns @{ moved; gaveBack }.
function Move-SrWindowToBottom([int]$ProcessId, [int]$TimeoutMs = 30000, [int]$GuardMs = 15000,
                               [IntPtr]$PreviousForeground = [IntPtr]::Zero) {
    $r = @{ moved = 0; gaveBack = 0 }
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        $r.moved = [SRBgWin]::ToBottom([uint32]$ProcessId)
        if ($r.moved -gt 0) { break }
        if (-not (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) { return $r }
        Start-Sleep -Milliseconds 50
    }
    if ($r.moved -eq 0) { return $r }
    $guardEnd = (Get-Date).AddMilliseconds($GuardMs)
    while ((Get-Date) -lt $guardEnd) {
        if ([SRBgWin]::ForegroundPid() -eq [uint32]$ProcessId) {
            if ([SRBgWin]::GiveBack($PreviousForeground)) { $r.gaveBack++ }
            [void][SRBgWin]::ToBottom([uint32]$ProcessId)
        }
        if (-not (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 100
    }
    return $r
}

# THE LIFETIME GUARD. A launched app may activate itself LATER than its first show (measured,
# iter-464 bgruns v2_xenia3: the stock Xenia took the foreground for one 0.5 s sample ~35 s
# after launch, while the user had the Start/Search flyout open - the shell lifts the
# foreground lock then). Until $ProcessId exits (or $Seconds > 0 pass) this polls every 50 ms:
# the window that last held the foreground (not $ProcessId) is remembered; when $ProcessId
# takes it WITHOUT a user gesture (mouse button / Alt / Win key down in the last 600 ms) the
# foreground is handed back to that window and $ProcessId's windows go to the bottom. A user
# who clicks or Alt+Tabs to the window keeps it until they leave it. Returns the hand-back count.
function Invoke-SrWindowGuard([int]$ProcessId, [int]$Seconds = 0, [string]$Log = "", [IntPtr]$PreviousForeground = [IntPtr]::Zero) {
    $deadline = $(if ($Seconds -gt 0) { (Get-Date).AddSeconds($Seconds) } else { [datetime]::MaxValue })
    # The hand-back target is NEVER a window of $ProcessId: seeded with the window that held the
    # foreground before the launch (the app may already own it by the time this starts).
    $lastOther = $PreviousForeground
    if ($lastOther -eq [IntPtr]::Zero -and [SRBgWin]::ForegroundPid() -ne [uint32]$ProcessId) { $lastOther = [SRBgWin]::GetForegroundWindow() }
    $lastGesture = [datetime]::MinValue
    $userOwned = $false
    $handled = $false   # one hand-back attempt per episode (a failed one is not retried in a loop)
    $gaveBack = 0
    $proc = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    while ($proc -and -not $proc.HasExited -and (Get-Date) -lt $deadline) {
        if ([SRBgWin]::GestureDown()) { $lastGesture = Get-Date }
        $fg = [SRBgWin]::GetForegroundWindow()
        $fgPid = [SRBgWin]::ForegroundPid()
        if ($fgPid -ne [uint32]$ProcessId) {
            if ($fg -ne [IntPtr]::Zero) { $lastOther = $fg }
            $userOwned = $false
            $handled = $false
        } elseif (-not $userOwned -and -not $handled) {
            if (((Get-Date) - $lastGesture).TotalMilliseconds -lt 600) {
                $userOwned = $true
                if ($Log) { Add-Content -Path $Log -Value ("{0} pid {1} activated by a user gesture - left alone" -f (Get-Date -Format 'HH:mm:ss.fff'), $ProcessId) -Encoding ascii }
            } else {
                $ok = [SRBgWin]::GiveBack($lastOther)
                [void][SRBgWin]::ToBottom([uint32]$ProcessId)
                $gaveBack++
                $handled = $true
                if ($Log) { Add-Content -Path $Log -Value ("{0} pid {1} TOOK the foreground; handed back: {2}" -f (Get-Date -Format 'HH:mm:ss.fff'), $ProcessId, $ok) -Encoding ascii }
            }
        }
        Start-Sleep -Milliseconds 50
    }
    return $gaveBack
}

# Starts a hidden guard process (Invoke-SrWindowGuard via -BgGuard) for the life of $ProcessId.
function Start-SrWindowGuard([int]$ProcessId, [string]$Log = "", [IntPtr]$PreviousForeground = [IntPtr]::Zero) {
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $cmd = ('"{0}" -NoProfile -ExecutionPolicy Bypass -File "{1}" -BgGuard {2} -BgPrevForeground {3}' -f $ps, $PSCommandPath, $ProcessId, $PreviousForeground.ToInt64()) + $(if ($Log) { ' -BgGuardLog ' + (ConvertTo-SrCommandLineArg $Log) } else { '' })
    $guardPid = [SRBgWin]::StartHidden($cmd, (Split-Path -Parent $PSCommandPath))
    return (Get-Process -Id $guardPid -ErrorAction SilentlyContinue)
}

# Starts OUR GAME (a CONSOLE-subsystem exe) for an automated run with NO console
# window: CreateNoWindow (CREATE_NO_WINDOW) and no show-window override. Start-Process
# gave every roll a new console, which Windows 11 opens as a Windows Terminal window
# that TAKES THE FOREGROUND (measured, iter-464 bgruns rollA: WindowsTerminal held it
# for 2.5 s at launch). The game window itself goes to the background on its own
# (SR_INSTANCE_ID; kernel/host_automation.hpp). The environment is inherited untouched
# (never copied, so a parent holding both PATH and Path is fine). -StdoutPath /
# -StderrPath copy the streams to files; call Complete-SrGameProcessOutput after the
# exit to flush them. Returns the started Process (handle open: ExitCode stays readable).
function Start-SrGameProcess([string]$FilePath, [string]$WorkingDirectory = "", [string]$ArgumentList = "",
                             [string]$StdoutPath = "", [string]$StderrPath = "") {
    # 2026-10-04: a lane's orphaned batch launched two rolls while the OWNER held the game slot for a
    # play-test. No automated launch may start while the slot belongs to the human owner
    # (tools/game_slot.py acquire --owner owner). Override only with SR_SLOT_IGNORE_OWNER=1.
    $ownerFile = Join-Path $(if ($env:SR_MAIN_ROOT) { $env:SR_MAIN_ROOT } else { Split-Path -Parent (git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir) }) "archive\game_slot.lock\owner.json"
    if ((Test-Path -LiteralPath $ownerFile) -and $env:SR_SLOT_IGNORE_OWNER -ne '1') {
        $slotOwner = $null
        try { $slotOwner = (Get-Content -LiteralPath $ownerFile -Raw | ConvertFrom-Json).owner } catch {}
        if ($slotOwner -eq 'owner') {
            throw "REFUSED: the game slot is held by the human owner (play-test); no automated launch of $FilePath"
        }
    }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($FilePath)
    if ($ArgumentList) { $psi.Arguments = $ArgumentList }
    $psi.WorkingDirectory = $(if ($WorkingDirectory) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($WorkingDirectory) } else { (Get-Location).ProviderPath })
    $psi.UseShellExecute = $false
    Add-SrSdlAudioEnvMirror $psi.EnvironmentVariables   # SDL2 + SDL3 audio env names (tools/sdl_audio_env.ps1)
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = [bool]$StdoutPath
    $psi.RedirectStandardError = [bool]$StderrPath
    $p = [System.Diagnostics.Process]::Start($psi)
    $null = $p.Handle
    $copies = @()
    foreach ($pair in @(@($StdoutPath, 'StandardOutput'), @($StderrPath, 'StandardError'))) {
        if (-not $pair[0]) { continue }
        $fs = [System.IO.File]::Create($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($pair[0]))
        $copies += @{ fs = $fs; task = $p.($pair[1]).BaseStream.CopyToAsync($fs) }
    }
    $p | Add-Member -NotePropertyName SrOutputCopies -NotePropertyValue $copies -Force
    return $p
}

# Flushes and closes the -StdoutPath / -StderrPath copies (after the process ended).
function Complete-SrGameProcessOutput($Process, [int]$TimeoutMs = 10000) {
    foreach ($c in @($Process.SrOutputCopies)) {
        if (-not $c) { continue }
        try { [void]$c.task.Wait($TimeoutMs) } catch {}
        try { $c.fs.Dispose() } catch {}
    }
}

# Polls the foreground owner every 0.5 s for $Seconds; writes one line per change
# (and a heartbeat every 10 s) to $Out. Returns the number of samples where a
# process named in $Flag held the foreground.
function Watch-SrForeground([string]$Out, [int]$Seconds, [string[]]$Flag) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    $hits = 0; $samples = 0; $last = -1; $beat = Get-Date
    $names = @{}
    Set-Content -Path $Out -Value ("# foreground watch start {0} flag={1}" -f (Get-Date -Format o), ($Flag -join ',')) -Encoding ascii
    while ((Get-Date) -lt $deadline) {
        $fgPid = [int][SRBgWin]::ForegroundPid()
        if (-not $names.ContainsKey($fgPid)) {
            $pn = (Get-Process -Id $fgPid -ErrorAction SilentlyContinue).ProcessName
            $names[$fgPid] = $(if ($pn) { $pn } else { '?' })
        }
        $name = $names[$fgPid]
        $samples++
        $flagged = $Flag -contains $name
        if ($flagged) { $hits++ }
        if ($fgPid -ne $last -or $flagged -or ((Get-Date) - $beat).TotalSeconds -ge 10) {
            Add-Content -Path $Out -Value ("{0} pid={1} name={2}{3}" -f (Get-Date -Format 'HH:mm:ss.fff'), $fgPid, $name, $(if ($flagged) { '  <== FLAGGED' } else { '' })) -Encoding ascii
            $last = $fgPid; $beat = Get-Date
        }
        Start-Sleep -Milliseconds 500
    }
    Add-Content -Path $Out -Value ("# foreground watch end {0}: samples={1} flaggedSamples={2}" -f (Get-Date -Format o), $samples, $hits) -Encoding ascii
    return $hits
}

if ($BgLaunch) {
    $tail = @($BgArgs) + @($BgRest | Where-Object { $null -ne $_ } | ForEach-Object { ConvertTo-SrCommandLineArg $_ })
    $p = Start-SrBackgroundProcess -FilePath $BgLaunch -ArgumentList (@($tail | Where-Object { $_ }) -join ' ') -WorkingDirectory $BgCwd
    $m = Move-SrWindowToBottom -ProcessId $p.Id -TimeoutMs 30000 -PreviousForeground $p.SrPrevForeground
    $guardNote = 'no lifetime guard (-BgNoGuard)'
    if (-not $BgNoGuard) { $g = Start-SrWindowGuard -ProcessId $p.Id -Log $BgGuardLog -PreviousForeground $p.SrPrevForeground; $guardNote = "lifetime guard PID $($g.Id)" }
    Write-Host ("launched PID {0} without activation; {1} window(s) moved to the bottom of the z-order; foreground handed back {2} time(s); {3}" -f $p.Id, $m.moved, $m.gaveBack, $guardNote)
    exit 0
}
if ($BgGuard) {
    $n = Invoke-SrWindowGuard -ProcessId $BgGuard -Seconds $(if ($PSBoundParameters.ContainsKey('BgSeconds')) { $BgSeconds } else { 0 }) -Log $BgGuardLog -PreviousForeground ([IntPtr]$BgPrevForeground)
    exit 0
}
if ($BgWatch) {
    $hits = Watch-SrForeground -Out $BgWatch -Seconds $BgSeconds -Flag @($BgFlag.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    Write-Host ("foreground watch: {0} flagged sample(s) -> {1}" -f $hits, $BgWatch)
    exit $(if ($hits -gt 0) { 1 } else { 0 })
}
