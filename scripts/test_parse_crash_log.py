#!/usr/bin/env python3
"""Crash-receipt regressions; fixtures only, no game launches or builds."""

from pathlib import Path
import tempfile
import unittest

import crash_markers
from parse_crash_log import parse_crash_log
import smoke_test


BOOT = "[2026-10-02 23:06:48.965] [ERROR] [BOOT] "
FAIL = (
    "[ICALL_FAIL] unresolvable guest indirect call: "
    "target=00000000 call_site=821673FC - stopping "
    "(SR_ALLOW_FORCING=1 restores the legacy r3=0 continue)"
)
SHORT_FAIL = "[ICALL_FAIL] target=00000000 call_site=821673FC"
UNRESOLVED = (
    "[ICALL_UNRESOLVED] #1 kind=null target=00000000 "
    "call_site=821673FC (lr=82167400) r3(obj)=00000000 "
    "vt=(unreadable)00000000 vt[+8]=00000000 vt[+0x18]=00000000 "
    "vt[+0x1C]=00000000 r4=00000000 r5=00000000 r1=70000000 "
    "ctr=00000000 tid=1234 -> "
)
REGS = (
    "[ICALL_UNRESOLVED] #1 regs r27=00000000 r28=00000000 "
    "r29=00000000 r30=00000000 r31=00000000 *r28=?00000000 "
    "*r31=?00000000 r11=00000000 r12=00000000"
)


class CrashLogTests(unittest.TestCase):
    def parse(self, text):
        with tempfile.TemporaryDirectory(prefix="sr_crash_receipt_") as root:
            path = Path(root) / "fixture.log"
            path.write_text(text, encoding="utf-8")
            return parse_crash_log(str(path)), smoke_test.parse_log(path)

    def assert_icall(self, text, call_site="0x821673FC", target="0x00000000"):
        result, smoke = self.parse(text)
        self.assertEqual(result["total_crashes"], 1)
        self.assertEqual(len(result["all_crashes"]), 1)
        crash = result["primary_crash"]
        self.assertEqual(crash["crash_type"], "icall_fail")
        self.assertEqual(crash["guest_call_site"], call_site)
        self.assertEqual(crash["indirect_call_target"], target)
        # A call instruction address is not a captured LR or exception address.
        self.assertEqual(crash["guest_lr"], "")
        self.assertEqual(crash["exception_code"], "")
        self.assertEqual(crash["fault_address"], "")
        self.assertEqual(
            result["investigation_targets"],
            [call_site] if int(call_site, 16) else [],
        )
        self.assertEqual([item["marker"] for item in smoke["fatal"]], ["icall_fail"])
        self.assertEqual(smoke_test.classify(smoke, smoke_test.DEFAULT_BASELINE)[0], "CRASH")
        return result

    def test_terminal_long_marker_bare_and_boot_prefixed(self):
        for prefix in ("", BOOT):
            with self.subTest(prefix=prefix):
                self.assert_icall(prefix + FAIL)

    def test_terminal_short_stderr_marker_bare_and_boot_prefixed(self):
        for prefix in ("", BOOT):
            with self.subTest(prefix=prefix):
                self.assert_icall(prefix + SHORT_FAIL)

    def test_diagnostics_plus_terminal_fail_count_only_the_fatal(self):
        result = self.assert_icall(
            BOOT + "[D3D_DIAG] present #240\n"
            + BOOT + "[RENDER_PUMP] state 2 -> 3 (phase=4)\n"
            + BOOT + UNRESOLVED + "FAIL\n"
            + BOOT + REGS + "\n"
            + BOOT + FAIL + "\n"
        )
        crash = result["primary_crash"]
        self.assertEqual(crash["last_present_number"], 240)
        self.assertEqual(crash["last_render_state"], "3")
        self.assertEqual(crash["last_render_phase"], "4")

    def test_unresolved_diagnostics_alone_are_not_fatal(self):
        for action in ("FAIL", "r3=0, continue (SR_ALLOW_FORCING legacy band-aid)"):
            with self.subTest(action=action):
                result, smoke = self.parse(BOOT + UNRESOLVED + action + "\n" + BOOT + REGS)
                self.assertEqual(result["total_crashes"], 0)
                self.assertIsNone(result["primary_crash"])
                self.assertEqual(result["investigation_targets"], [])
                self.assertEqual(smoke["fatal"], [])

    def test_nonzero_indirect_target_is_not_an_exception_address(self):
        self.assert_icall(
            SHORT_FAIL.replace("target=00000000", "target=deadbeef"),
            target="0xDEADBEEF",
        )

    def test_unknown_call_site_is_recorded_but_not_an_investigation_target(self):
        self.assert_icall(
            SHORT_FAIL.replace("call_site=821673FC", "call_site=00000000"),
            call_site="0x00000000",
        )

    def test_incomplete_marker_is_not_fatal(self):
        for marker in (
            "[ICALL_FAIL]",
            "[ICALL_FAIL] target=00000000",
            "[ICALL_FAIL] target=00000000 call_site=821673F",
            "[ICALL_FAIL] target=00000000 call_site=821673FC0",
        ):
            with self.subTest(marker=marker):
                self.assertIsNone(crash_markers.scan_fatal(marker))
                result, _ = self.parse(marker)
                self.assertEqual(result["total_crashes"], 0)

    def test_existing_main_exception_context_is_unchanged(self):
        result, _ = self.parse(
            "[D3D_DIAG] present #300\n"
            "Guest harness: exception 0xC0000005 at host IP 0x00007FF6ABCD1234\n"
            "guest ctx: lr=0x82282DF0 r1=0x70000000 r3=0x00000000\n"
            "read at data address 0x00000010\n"
            "fault outside guest memory\n"
        )
        self.assertEqual(result["total_crashes"], 1)
        crash = result["primary_crash"]
        self.assertEqual(crash["crash_type"], "main_exception")
        self.assertEqual(crash["exception_code"], "0xC0000005")
        self.assertEqual(crash["host_ip"], "0x00007FF6ABCD1234")
        self.assertEqual(crash["guest_lr"], "0x82282DF0")
        self.assertEqual(crash["guest_r1"], "0x70000000")
        self.assertEqual(crash["guest_r3"], "0x00000000")
        self.assertEqual(crash["fault_address"], "0x00000010")
        self.assertEqual(crash["fault_type"], "outside_guest")
        self.assertEqual(crash["last_present_number"], 300)
        self.assertEqual(result["investigation_targets"], ["0x82282DF0"])

    def test_existing_thread_and_final_markers_are_unchanged(self):
        result, _ = self.parse(
            "[ExCreateThread] guest thread startVA=0x82A1B000 exception code=0xC0000005 "
            "at host=00007FF6ABCD5678 fault=0x00000010 lr=0x8267F478 "
            "r3=0x00000000 r1=0x71000000 r31=0x70123400\n"
            "[CRASH] Exception 0xC0000005 at 00007FF6ABCDEF00 (thread 7)\n"
        )
        self.assertEqual(result["total_crashes"], 2)
        thread, final = result["all_crashes"]
        self.assertEqual(thread["crash_type"], "thread_exception")
        self.assertEqual(thread["guest_r31"], "0x70123400")
        self.assertEqual(final["crash_type"], "final_crash")
        self.assertEqual(final["thread_id"], "7")
        self.assertEqual(result["investigation_targets"], ["0x8267F478"])


if __name__ == "__main__":
    unittest.main()
