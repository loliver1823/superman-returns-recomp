#!/usr/bin/env python3
"""direct_calls_guard.py <guest/ppc dir> - fail the build when a direct-call edge would bypass an override.

tools/regen/direct_calls.py rewrites hot guest call edges to call __imp__sub_X (the body) instead of
sub_X (the weak alias a kernel strong definition replaces) and lists those callees in
<dir>/direct_calls.list. If any of them has an override now - registered in docs/OVERRIDES.json or a
strong definition in kernel/ guest/ gfx/ app/ - those edges would silently skip it. Exit 1 then,
naming the functions and the fix (`python tools/regen/direct_calls.py revert guest/ppc`, or drop
the edge and re-apply). No list = nothing converted = pass. Run by every build of the guest lib
(guest/CMakeLists.txt, target direct_calls_guard).
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import direct_calls  # noqa: E402

d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(direct_calls.ROOT, "guest", "ppc")
lp = os.path.join(d, direct_calls.LIST)
if not os.path.exists(lp):
    sys.exit(0)
callees = [l.strip() for l in open(lp, encoding="ascii") if l.strip()]
bad = sorted(set(callees) & direct_calls.overridden())
if bad:
    sys.stderr.write("direct_calls_guard: FAIL - these callees now have an override, and guest/ppc calls "
                     "their bodies directly on hot edges (the override would be bypassed):\n  %s\n"
                     "Fix: python tools/regen/direct_calls.py revert guest/ppc (or remove the edges from "
                     "tools/regen/direct_calls_edges.json and apply again).\n" % "\n  ".join(bad))
    sys.exit(1)
print("direct_calls_guard: %d direct-call callees, none overridden" % len(callees))
