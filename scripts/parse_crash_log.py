#!/usr/bin/env python3
"""parse_crash_log.py — Extract structured crash info from Superman Returns recomp logs.

Usage:
    python parse_crash_log.py <crash.log> [debug.json.log]

Outputs JSON with crash details, guest context, watchpoint hits,
kernel calls, and progress metrics.
"""

import re
import json
import sys
import os
from dataclasses import dataclass, field, asdict
from typing import Optional

# Single source of truth for the fatal crash-marker regexes (shared with
# scripts/smoke_test.py so detection can never diverge). See crash_markers.py.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import crash_markers


@dataclass
class CrashInfo:
    exception_code: str = ""
    host_ip: str = ""
    thread_id: str = ""
    guest_lr: str = ""
    guest_r1: str = ""
    guest_r3: str = ""
    guest_r31: str = ""
    fault_address: str = ""
    fault_type: str = ""  # "inside_guest" or "outside_guest"
    source_file: str = ""
    source_line: str = ""
    crash_type: str = ""  # main_exception, thread_exception, final_crash, icall_fail
    thread_start_va: str = ""  # for thread crashes
    last_present_number: int = 0
    last_render_state: str = ""
    last_render_phase: str = ""
    watchpoint_hits: list = field(default_factory=list)
    kernel_calls_before_crash: list = field(default_factory=list)
    guest_call_site: str = ""  # ICALL_FAIL instruction VA, not a captured LR
    indirect_call_target: str = ""  # ICALL_FAIL target, not an AV fault address


def parse_crash_log(log_path: str) -> dict:
    """Parse a runtime crash log into structured crash data."""
    with open(log_path) as f:
        lines = f.readlines()

    all_crashes = []
    watchpoints = []
    kernel_calls = []
    render_transitions = []
    worker_traces = []
    last_present = 0
    last_render_state = ""
    last_render_phase = ""

    for line in lines:
        # Track present count (progress metric)
        present_match = crash_markers.PRESENT.search(line)
        if present_match:
            last_present = int(present_match.group(1))

        # Track render pump state
        render_match = re.search(
            r'\[RENDER_PUMP\] state (\d+) -> (\d+) \(phase=(\d+)\)', line
        )
        if render_match:
            last_render_state = render_match.group(2)
            last_render_phase = render_match.group(3)
            render_transitions.append({
                "from": render_match.group(1),
                "to": render_match.group(2),
                "phase": render_match.group(3)
            })

        # Track worker traces
        worker_match = re.search(
            r'\[WORKER_TRACE\] (\w+) val1=(0x[0-9A-Fa-f]+) val2=(0x[0-9A-Fa-f]+) \(call#(\d+)\)',
            line
        )
        if worker_match:
            worker_traces.append({
                "label": worker_match.group(1),
                "val1": worker_match.group(2),
                "val2": worker_match.group(3),
                "call_num": int(worker_match.group(4))
            })

        # Collect watchpoint hits
        wp_match = re.search(
            r'\[WATCHPOINT\] (\w+ write) @(0x[0-9A-Fa-f]+) = (0x[0-9A-Fa-f]+) '
            r'at (.+?):(\d+) guestLR=(0x[0-9A-Fa-f]+)(?: \(hit #(\d+)\))?',
            line
        )
        if wp_match:
            watchpoints.append({
                "type": wp_match.group(1),
                "address": wp_match.group(2),
                "value": wp_match.group(3),
                "source": f"{wp_match.group(4)}:{wp_match.group(5)}",
                "guest_lr": wp_match.group(6),
                "hit_num": int(wp_match.group(7)) if wp_match.group(7) else None
            })

        # Collect kernel import calls
        kernel_match = re.search(
            r'\[KERNEL:IMPORT\] (\w+)\(.*?\)\('
            r'(r3=[0-9A-Fa-f]+ .*?)\) -> ([0-9A-Fa-f]+)',
            line
        )
        if kernel_match:
            kernel_calls.append({
                "function": kernel_match.group(1),
                "args": kernel_match.group(2),
                "result": kernel_match.group(3)
            })

        # Also catch kernel calls without the arg→result pattern (VdSetDisplayMode etc.)
        kernel_noret = re.search(
            r'\[KERNEL:IMPORT\] (\w+)\((r3=0x[0-9A-Fa-f]+.*?)\)\s*$', line
        )
        if kernel_noret and not kernel_match:
            kernel_calls.append({
                "function": kernel_noret.group(1),
                "args": kernel_noret.group(2),
                "result": None
            })

        # Fallback index hints (e.g., "8278C840_fallback_to_idx0")
        fallback_match = re.search(
            r'\[SUB\w+\] (\w+_fallback\w*): val1=(0x[0-9A-Fa-f]+) val2=(0x[0-9A-Fa-f]+)',
            line
        )
        if fallback_match:
            worker_traces.append({
                "label": fallback_match.group(1),
                "val1": fallback_match.group(2),
                "val2": fallback_match.group(3),
                "call_num": -1  # sentinel for fallback
            })

        # === CRASH DETECTION ===

        # A terminal indirect-call failure has no host exception/guest-ctx
        # trailer. Count only ICALL_FAIL, not its ICALL_UNRESOLVED diagnostics
        # (those can also accompany the opt-in nonfatal forcing path).
        icall_fail = crash_markers.ICALL_FAIL.search(line)
        if icall_fail:
            c = CrashInfo()
            c.crash_type = "icall_fail"
            c.indirect_call_target = "0x" + icall_fail.group(1).upper()
            c.guest_call_site = "0x" + icall_fail.group(2).upper()
            c.last_present_number = last_present
            c.last_render_state = last_render_state
            c.last_render_phase = last_render_phase
            c.watchpoint_hits = watchpoints[-10:]
            c.kernel_calls_before_crash = kernel_calls[-5:]
            all_crashes.append(c)
            continue

        # Main guest exception (access violation mid-execution)
        guest_exc = crash_markers.GUEST_HARNESS_EXCEPTION.search(line)
        if guest_exc:
            c = CrashInfo()
            c.exception_code = guest_exc.group(1)
            c.host_ip = guest_exc.group(2)
            c.crash_type = "main_exception"
            c.last_present_number = last_present
            c.last_render_state = last_render_state
            c.last_render_phase = last_render_phase
            c.watchpoint_hits = watchpoints[-10:]
            c.kernel_calls_before_crash = kernel_calls[-5:]
            all_crashes.append(c)
            continue

        # Guest context line (follows the exception line)
        ctx_match = re.search(
            r'guest ctx: lr=(0x[0-9A-Fa-f]+) r1=(0x[0-9A-Fa-f]+) '
            r'r3=(0x[0-9A-Fa-f]+)',
            line
        )
        if ctx_match and all_crashes:
            all_crashes[-1].guest_lr = ctx_match.group(1)
            all_crashes[-1].guest_r1 = ctx_match.group(2)
            all_crashes[-1].guest_r3 = ctx_match.group(3)

        # Fault location detail
        fault_match = re.search(
            r'(write|read) at data address (0x[0-9A-Fa-f]+)', line
        )
        if fault_match and all_crashes:
            all_crashes[-1].fault_address = fault_match.group(2)

        if re.search(r'fault outside guest memory', line) and all_crashes:
            all_crashes[-1].fault_type = "outside_guest"

        if re.search(r'fault inside guest memory', line) and all_crashes:
            all_crashes[-1].fault_type = "inside_guest"

        # Source file reference from watchpoint or crash context
        source_match = re.search(
            r'at .+?/(ppc_recomp\.\d+\.cpp):(\d+)', line
        )
        if source_match and all_crashes and not all_crashes[-1].source_file:
            all_crashes[-1].source_file = source_match.group(1)
            all_crashes[-1].source_line = source_match.group(2)

        # ExCreateThread crash (spawned thread exception)
        thread_crash = crash_markers.EXCREATETHREAD_CRASH.search(line)
        if thread_crash:
            c = CrashInfo()
            c.crash_type = "thread_exception"
            c.thread_start_va = thread_crash.group(1)
            c.exception_code = thread_crash.group(2)
            c.host_ip = thread_crash.group(3)
            c.fault_address = thread_crash.group(4)
            c.guest_lr = thread_crash.group(5)
            c.guest_r3 = thread_crash.group(6)
            c.guest_r1 = thread_crash.group(7)
            c.last_present_number = last_present
            c.last_render_state = last_render_state
            c.last_render_phase = last_render_phase
            c.kernel_calls_before_crash = kernel_calls[-5:]
            all_crashes.append(c)

            # Try to get r31 if present
            r31_match = re.search(r'r31=(0x[0-9A-Fa-f]+)', line)
            if r31_match:
                c.guest_r31 = r31_match.group(1)

        # Final crash line
        final_crash = crash_markers.FINAL_CRASH.search(line)
        if final_crash:
            c = CrashInfo()
            c.crash_type = "final_crash"
            c.exception_code = final_crash.group(1)
            c.host_ip = final_crash.group(2)
            c.thread_id = final_crash.group(3)
            c.last_present_number = last_present
            all_crashes.append(c)

    # Deduplicate watchpoint sources for summary
    wp_sources = {}
    for wp in watchpoints:
        key = wp["guest_lr"]
        if key not in wp_sources:
            wp_sources[key] = {"guest_lr": key, "source": wp["source"], "count": 0}
        wp_sources[key]["count"] += 1

    result = {
        "total_crashes": len(all_crashes),
        "max_present_reached": last_present,
        "primary_crash": asdict(all_crashes[0]) if all_crashes else None,
        "all_crashes": [asdict(c) for c in all_crashes],
        "total_watchpoint_hits": len(watchpoints),
        "watchpoint_summary": list(wp_sources.values()),
        "last_watchpoints": watchpoints[-5:] if watchpoints else [],
        "render_transitions": render_transitions[-5:] if render_transitions else [],
        "worker_traces": worker_traces[-5:] if worker_traces else [],
        "investigation_targets": []
    }

    # Build investigation target list (unique guest addresses to look up in Ghidra)
    targets = set()
    for c in all_crashes:
        if c.guest_lr:
            targets.add(c.guest_lr)
        if c.guest_call_site and int(c.guest_call_site, 16):
            targets.add(c.guest_call_site)
    for wp_src in wp_sources.values():
        targets.add(wp_src["guest_lr"])
    result["investigation_targets"] = sorted(targets)

    return result


def parse_debug_json_log(log_path: str) -> dict:
    """Parse the structured JSON debug log for render pipeline state."""
    entries = []

    with open(log_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                entry = json.loads(line)
                entries.append(entry)
            except json.JSONDecodeError:
                continue

    if not entries:
        return {"total_entries": 0}

    present_counts = []
    for e in entries:
        d = e.get("data", {})
        val = d.get("presentN") or d.get("presentCount")
        if val is not None:
            present_counts.append(val)

    return {
        "total_entries": len(entries),
        "duration_ms": entries[-1]["timestamp"] - entries[0]["timestamp"],
        "max_present": max(present_counts) if present_counts else 0,
        "unique_locations": list(set(e["location"] for e in entries)),
        "unique_messages": list(set(e["message"] for e in entries)),
        "last_entries": entries[-3:],
    }


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: parse_crash_log.py <crash.log> [debug.json.log]")
        sys.exit(1)

    crash_result = parse_crash_log(sys.argv[1])
    print(json.dumps(crash_result, indent=2))

    if len(sys.argv) >= 3:
        print("\n--- DEBUG LOG SUMMARY ---")
        debug_result = parse_debug_json_log(sys.argv[2])
        print(json.dumps(debug_result, indent=2))
