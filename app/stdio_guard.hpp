// iter-465 player: THE STDIO GUARD - no game thread ever blocks on a stdout/stderr sink.
//
// THE DEATH IT FIXES (archive/player_integration_20261002/interactive1, 2026-10-02 23:53): the
// owner's MODS-menu RESTART NOW relaunch froze ~22 s into the new boot (EA logo; logging AND
// presenting stopped at 23:53:38.2). Mechanism, from the receipts: the launcher redirected the
// game's stderr into an anonymous pipe (tools/bg_window.ps1 Start-SrGameProcess); the relaunched
// child inherited that pipe; when the FIRST process exited, the launcher's
// Complete-SrGameProcessOutput waited 2 x 10 s and then disposed its copy sinks, so nobody read
// the pipe any more. sr::Log::writeV mirrors every line to stderr with a synchronous WriteFile
// while holding the global log spinlock, so the first write into the full pipe blocked forever
// and every other logging thread (render, guest, sampler) spun on the lock: a frozen game.
// stderr.log stops at 23:53:38.177, the child's file log at 23:53:38.200 (the lines that still
// fit in the pipe buffer), 21.3 s after the parent's exit (= the two 10 s waits + the 1 s poll).
//
// Any stdio sink that can stall has the same failure: a pipe nobody drains, a console in
// QuickEdit selection mode (the exe is a console-subsystem program, so a double-click opens one),
// a slow terminal. Guest bodies print to the CRT stderr too (fprintf in spliced recomp bodies),
// so fixing only the logger is not enough.
//
// THE FIX (host platform; nothing guest-visible): when STD_OUTPUT / STD_ERROR is a pipe or a
// character device, install() interposes a private pipe (1 MB) in front of it - SetStdHandle and
// the CRT's fd 1/2 both point at it - and two host threads forward it: a reader that always
// drains the private pipe into a bounded ring (16 MB; it DROPS and counts when the ring is full),
// and a writer that copies the ring to the original sink (the only thread that can block on it).
// Disk files are left alone (a file write never stalls indefinitely). SR_STDIO_GUARD=0 disables.
#pragma once

#include <cstdint>

namespace sr::stdio_guard {

struct Stats {
  bool stdoutGuarded = false;
  bool stderrGuarded = false;
  unsigned stdoutType = 0;   // GetFileType of the original sink (2 = char/console, 3 = pipe)
  unsigned stderrType = 0;
  uint64_t forwardedBytes = 0;
  uint64_t droppedBytes = 0;
  bool sinkFailed = false;   // a WriteFile to an original sink failed (reader gone)
};

// Once, first thing in main (before the log opens). Idempotent.
void install();
// Best-effort: waits up to timeoutMs for everything written so far to reach the original sinks.
void flush(unsigned timeoutMs);
Stats stats();

// iter-465 player: a console-subsystem exe started by a double-click gets a console window of
// its own. When this process is the ONLY one attached to its console (nobody launched us from a
// terminal) and the run is not automated, detach from it: the player sees the game window only,
// and the log keeps going to its file. Returns true when it detached. SR_KEEP_CONSOLE=1 keeps it.
bool detachOwnConsoleForPlayer(bool automated);

}  // namespace sr::stdio_guard
