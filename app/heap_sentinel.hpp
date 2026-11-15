// step4/relcrash: the LFH UserBlocks sentinel (SR_HEAP_SENTINEL, default OFF; a diagnostic).
//
// Why: the release whole-game run s4h_wg_rel1 died in RtlFreeHeap because the first qword of an LFH UserBlocks
// header (_HEAP_USERDATA_HEADER.SubSegment) held 0x0011111100101010 - a stray host write, detected only when a
// later free walked into it (the XMA decoder's codec close). HeapValidate does not see that clobber (tested,
// archive/step4_claude_20261018/relcrash/NOTES.md), so this thread watches the headers themselves:
//   - discovery (every SR_HEAP_SENTINEL_SCAN_MS, default 5000): committed MEM_PRIVATE PAGE_READWRITE regions outside
//     the guest arena are copied in 64 KB chunks (ReadProcessMemory: never faults, the harness VEH rule) and every
//     16-byte slot whose +0x14 dword is the UserBlocks signature 0xF0E0D0C0 and whose +0 qword is a plausible user
//     pointer is adopted, with the 16 bytes of the backend heap entry in front of it;
//   - check (every SR_HEAP_SENTINEL=<ms> ms, 10..10000; "1" = 100): each adopted header is re-read; signature gone =
//     the UserBlocks went away (dropped); +0 changed to another plausible pointer = legitimate (followed); +0
//     IMPLAUSIBLE (non-canonical, unaligned, below 64 KB) twice 2 ms apart = a clobber:
//       DETECTED when the backend entry in front is unchanged (the UserBlocks is still the same live block),
//       SUSPECT when it changed (the block may have been freed and reused);
//     each logs [HEAP_SENTINEL] with the address, both values, the time since the last clean check of that header,
//     the region, and 0x80 bytes around it; the first DETECTED and the first SUSPECT also write a heap minidump
//     (writeDiagnosticMinidump(..., withHeap)) next to the crash dump.
//   - [HEAP_SENTINEL] census every 60 s (headers known, scan / check cost, drops, follows, events).
// SR_HEAP_SENTINEL_SELFTEST=1: after the first discovery the thread builds a private LFH heap, clobbers one of its
// UserBlocks headers with the s4h_wg_rel1 pattern, requires its own check to report it (no dump), restores it and
// destroys the heap: "[HEAP_SENTINEL] selftest PASS|FAIL" - the activation witness for this exe on this machine.
// Observe-only: it never writes process memory except the self-test's own private heap.
#pragma once

namespace sr {

// Starts the sentinel thread when SR_HEAP_SENTINEL is set (a no-op otherwise). Call once, after
// installCrashHandler(). It waits for the guest arena to exist (g_debugGuestBase) before its first scan.
void startHeapSentinel();

// difftest --heap-dump-test: run the self-test on the calling thread; mutation 1 = the sentinel can never fire (its
// plausibility check accepts everything), so the self-test must FAIL. Returns the self-test's verdict.
bool heapSentinelSelfTestForTest(int mutation);

}  // namespace sr
