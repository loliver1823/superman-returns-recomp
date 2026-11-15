#!/usr/bin/env python3
"""Fixture tests for scripts/audit_host_threads.py (iter-463).

Each case is a tiny C++ TU in a throwaway tree; the test asserts the status the
audit gives its launch site. The cases are the shapes that let forcing ship past
audit_forcing.py (a raw store through gb+VA, a ReleaseSemaphore, a gate OR'd with
forcingAllowed(), a gate inside the thread body instead of at the launch) plus
the shapes that must NOT fail (read-only observers, master-gated launches,
registered platform threads). Run: python scripts/test_audit_host_threads.py
"""
import io
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audit_host_threads as aht  # noqa: E402

PRE = "#include <thread>\nnamespace sr { namespace kernel { bool forcingAllowed(); } }\n"

CASES = [
    # (name, code, registry-or-None, expected status of the (first) launch)
    ("raw store ungated", PRE + """
void pokeThread(uint8_t* gb) { uint8_t* h = gb + 0x8296D874u; h[0] = 0; }
void start(uint8_t* gb) { std::thread(pokeThread, gb).detach(); }
""", None, "FAIL"),
    ("raw store in forcingAllowed block", PRE + """
void pokeThread(uint8_t* gb) { uint8_t* h = gb + 0x8296D874u; h[0] = 0; }
void start(uint8_t* gb) {
  if (sr::kernel::forcingAllowed()) { std::thread(pokeThread, gb).detach(); }
}
""", None, "MASTER"),
    ("launch after the gated block closes", PRE + """
void pokeThread(uint8_t* gb) { uint8_t* h = gb + 0x10u; h[0] = 0; }
void other(uint8_t* gb) { uint8_t* h = gb + 0x20u; h[0] = 0; }
void start(uint8_t* gb) {
  if (sr::kernel::forcingAllowed()) { std::thread(other, gb).detach(); }
  std::thread(pokeThread, gb).detach();
}
""", None, "MASTER,FAIL"),
    ("OR-bypass of the master gate", PRE + """
#include <windows.h>
static bool s_own = true;
void create(HANDLE h, bool pat) {
  if (pat && (s_own || sr::kernel::forcingAllowed())) {
    std::thread([h]() { ReleaseSemaphore(h, 1, nullptr); }).detach();
  }
}
""", None, "FAIL"),
    ("early-return guard", PRE + """
void pokeThread(uint8_t* gb) { writeGuestBE32(0x8296D8D8u, 0); }
void start(uint8_t* gb) {
  if (!sr::kernel::forcingAllowed()) return;
  std::thread(pokeThread, gb).detach();
}
""", None, "MASTER"),
    ("gate only inside the thread body", PRE + """
void pokeThread(uint8_t* gb) {
  if (!sr::kernel::forcingAllowed()) return;
  GuestCaller gc("X");
}
struct Starter { Starter() { std::thread(pokeThread, nullptr).detach(); } } s_starter;
""", None, "FAIL"),
    ("captured guest pointer in a lambda", PRE + """
void start(uint8_t* g_guestBase) {
  uint8_t* bundle = g_guestBase + 0xD0012000u;
  std::thread([bundle] { for (;;) { bundle[0] = 1; } }).detach();
}
""", None, "FAIL"),
    ("read-only observer", PRE + """
void watch(uint8_t* gb) {
  const uint8_t* p = gb + 0x8296D8D8u;
  uint32_t v = p[0];
  (void)v;
}
void start(uint8_t* gb) { std::thread(watch, gb).detach(); }
""", None, "INERT"),
    ("master predicate function", PRE + """
bool myGate() { return sr::kernel::forcingAllowed() && true; }
void pokeThread(uint8_t* gb) { PPC_STORE_U32(0x10, 0); }
void start(uint8_t* gb) { if (myGate()) std::thread(pokeThread, gb).detach(); }
""", None, "MASTER"),
    ("B1: same-named own-gate in another namespace is not the master", PRE + """
namespace a { bool gate() { return sr::kernel::forcingAllowed() && true; } }
namespace b { bool gate() { return true; } }
void pokeThread(uint8_t* gb) { PPC_STORE_U32(0x10, 0); }
void start(uint8_t* gb) { if (b::gate()) std::thread(pokeThread, gb).detach(); }
""", None, "FAIL"),
    # iter-463 round 4 (H2, verifier p8): two shapes read MASTER while the launch
    # runs with forcing OFF; the controls keep the real master forms MASTER.
    ("H2: else-if early return guards one branch only", PRE + """
void logit();
void pokeThread(uint8_t* gb) { uint8_t* h = gb + 0x10u; h[0] = 0; }
void start(uint8_t* gb, bool a) {
  if (a) { logit(); } else if (!sr::kernel::forcingAllowed()) return;
  std::thread(pokeThread, gb).detach();
}
""", None, "FAIL"),
    ("H2: `forcingAllowed() == false` is the negation", PRE + """
void pokeThread(uint8_t* gb) { uint8_t* h = gb + 0x10u; h[0] = 0; }
void start(uint8_t* gb) {
  if (sr::kernel::forcingAllowed() == false) { std::thread(pokeThread, gb).detach(); }
}
""", None, "FAIL"),
    ("H2 control: `== false` early return is a real guard", PRE + """
void pokeThread(uint8_t* gb) { uint8_t* h = gb + 0x10u; h[0] = 0; }
void start(uint8_t* gb) {
  if (sr::kernel::forcingAllowed() == false) return;
  std::thread(pokeThread, gb).detach();
}
""", None, "MASTER"),
    ("H2 control: `!= false` block is master-gated", PRE + """
void pokeThread(uint8_t* gb) { uint8_t* h = gb + 0x10u; h[0] = 0; }
void start(uint8_t* gb) {
  if (sr::kernel::forcingAllowed() != false) { std::thread(pokeThread, gb).detach(); }
}
""", None, "MASTER"),
    ("transitive helper write", PRE + """
static void helper(uint32_t va) { writeGuest32(va, 1); }
void loopThread() { for (;;) helper(0x9480C000u); }
void start() { std::thread(loopThread).detach(); }
""", None, "FAIL"),
    ("nested launch is audited separately", PRE + """
void inner(uint8_t* gb) { uint8_t* h = gb + 0x10u; h[0] = 0; }
void start(uint8_t* gb) {
  std::thread([gb] { int x = 0; (void)x; std::thread(inner, gb).detach(); }).detach();
}
""", None, "INERT,FAIL"),
    ("allowlisted platform thread", PRE + """
void isrThread() { GuestCaller gc("VD_ISR"); }
void start() { std::thread(isrThread).detach(); }
""", {"allow": [{"file": "kernel/t.cpp", "entry": "isrThread", "class": "platform", "reason": "r"}]},
     "ALLOW"),
    ("allowlisted, forcing branch predicate not master", PRE + """
bool forceOn() { return true; }
void deliver() { xnEnqueue(10, 1); if (!forceOn()) return; xnEnqueue(9, 0); }
void start() { std::thread(deliver).detach(); }
""", {"allow": [{"file": "kernel/t.cpp", "entry": "deliver", "class": "platform", "reason": "r",
                 "must_gate": ["forceOn"]}]}, "FAIL"),
    ("allowlisted, forcing branch predicate master", PRE + """
bool forceOn() { return sr::kernel::forcingAllowed() && true; }
void deliver() { xnEnqueue(10, 1); if (!forceOn()) return; xnEnqueue(9, 0); }
void start() { std::thread(deliver).detach(); }
""", {"allow": [{"file": "kernel/t.cpp", "entry": "deliver", "class": "platform", "reason": "r",
                 "must_gate": ["forceOn"]}]}, "ALLOW"),
    ("observe claim pattern removed", PRE + """
void patcher() { constexpr bool kObserveOnly = false; if (!kObserveOnly) writeGuestBE32(0x10, 1); }
void start() { std::thread(patcher).detach(); }
""", {"allow": [{"file": "kernel/t.cpp", "entry": "patcher", "class": "diagnostic", "reason": "r",
                 "requires": ["constexpr bool kObserveOnly = true"]}]}, "FAIL"),
    ("forcing class cannot be allowlisted", PRE + """
void pokeThread(uint8_t* gb) { uint8_t* h = gb + 0x10u; h[0] = 0; }
void start(uint8_t* gb) { std::thread(pokeThread, gb).detach(); }
""", {"allow": [{"file": "kernel/t.cpp", "entry": "pokeThread", "class": "forcing", "reason": "r"}]},
     "FAIL"),
]

SITE_CASES = [
    ("site ungated", "void f(uint8_t* qf) { *qf = 0; }\n", "OPEN"),
    ("site gated", PRE + "void f(uint8_t* qf) { if (sr::kernel::forcingAllowed()) { *qf = 0; } }\n", "FIXED"),
    ("site deleted", "void f(uint8_t* qf) { (void)qf; }\n", "FIXED"),
]


def run_case(code, registry):
    root = tempfile.mkdtemp(prefix="aht_")
    try:
        os.makedirs(os.path.join(root, "kernel"))
        os.makedirs(os.path.join(root, "docs"))
        with open(os.path.join(root, "kernel", "t.cpp"), "w", encoding="utf-8") as fh:
            fh.write(code)
        with open(os.path.join(root, "docs", "HOST_THREADS.json"), "w", encoding="utf-8") as fh:
            json.dump(registry or {"allow": [], "open_forcing": []}, fh)
        buf = io.StringIO()
        rc = aht.run(root, "--json", out=buf)
        return rc, json.loads(buf.getvalue())
    finally:
        shutil.rmtree(root, ignore_errors=True)


def main():
    failed = 0
    for name, code, reg, want in CASES:
        _rc, res = run_case(code, reg)
        got = ",".join(l["status"] for l in sorted(res["launches"], key=lambda l: l["line"]))
        ok = got == want
        failed += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} {name}: got {got or '(none)'} want {want}")
    for name, code, want in SITE_CASES:
        reg = {"allow": [], "open_forcing": [{"id": "s", "kind": "site", "file": "kernel/t.cpp",
                                               "anchor": r"\*qf = 0;", "what": "w", "recommendation": "r"}]}
        rc, res = run_case(code, reg)
        got = res["open_sites"][0]["status"]
        ok = got == want and (rc == 1) == (want == "OPEN")
        failed += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} {name}: got {got} rc={rc} want {want}")
    print(f"{len(CASES) + len(SITE_CASES) - failed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
