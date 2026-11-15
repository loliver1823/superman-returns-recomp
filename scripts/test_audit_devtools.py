#!/usr/bin/env python3
"""Mutation tests for the DEV-TOOL rules of scripts/audit_forcing.py (iter-463 phase 2,
docs/DEVTOOLS_DESIGN.md 2.1 / 2.2; the site-level DEVTOOL level itself is fixture d40_devtool
of scripts/test_audit_forcing.py).

Each case builds a small tree - the REAL kernel/devtools.cpp, kernel/devtools_levelskip.cpp and
docs/DEVTOOLS.json, plus one planted file or edit - runs audit_forcing.collect() and
devtool_report(), and checks the verdict: the control tree must be clean, every planted
violation must be reported. Also checks the live repository is clean and that the host-thread
registry carries ZERO devtool rows.

    python scripts/test_audit_devtools.py        (exit = failed cases)
"""
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import audit_forcing as af  # noqa: E402

REAL = ["kernel/devtools.cpp", "kernel/devtools_levelskip.cpp", "kernel/devtools.hpp", "docs/DEVTOOLS.json"]

SITE_OK = '''#include <cstdint>
namespace sr { namespace kernel { bool devToolAllowed(const char*); void writeGuestBE32(uint32_t, uint32_t); } }
struct PPCContext { struct { uint32_t u32; } r3; };
void toolWrite(uint32_t va) {
  if (!sr::kernel::devToolAllowed("levelskip")) return;
  sr::kernel::writeGuestBE32(va, 1u);
}
void sub_82000050(PPCContext& ctx, uint8_t* base) { toolWrite(ctx.r3.u32); }
'''


def build(tmp, extra=None, edits=None, json_edit=None):
    root = tempfile.mkdtemp(dir=tmp)
    for rel in REAL:
        os.makedirs(os.path.join(root, os.path.dirname(rel)), exist_ok=True)
        shutil.copy(os.path.join(ROOT, rel), os.path.join(root, rel))
    with open(os.path.join(root, "kernel", "site.cpp"), "w", encoding="utf-8") as f:
        f.write(SITE_OK)
    for rel, text in (extra or {}).items():
        os.makedirs(os.path.join(root, os.path.dirname(rel)), exist_ok=True)
        with open(os.path.join(root, rel), "w", encoding="utf-8") as f:
            f.write(text)
    for rel, (old, new) in (edits or {}).items():
        p = os.path.join(root, rel)
        s = open(p, encoding="utf-8").read()
        assert s.count(old) >= 1, (rel, old)
        open(p, "w", encoding="utf-8").write(s.replace(old, new, 1))
    if json_edit:
        p = os.path.join(root, "docs", "DEVTOOLS.json")
        d = json.load(open(p, encoding="utf-8"))
        json_edit(d)
        json.dump(d, open(p, "w", encoding="utf-8"))
    return root


def verdict(root):
    parsed, sites = af.collect(root)
    return af.devtool_report(parsed, root, sites, quiet=True), sites


def drop_row(name):
    return lambda d: d.__setitem__("tools", [r for r in d["tools"] if r["name"] != name])


def set_class(name, cls):
    def f(d):
        for r in d["tools"]:
            if r["name"] == name:
                r["class"] = cls
    return f


CASES = [
    # (name, kwargs for build, substring every reported violation list must contain or None = clean)
    ("control: the real files + a gated write", {}, None),
    ("env read outside devtools.cpp", {"extra": {"kernel/x.cpp": '#include <cstdlib>\nconst char* g() { return std::getenv("SR_DEVTOOLS_CMDS"); }\n'}},
     "reads SR_DEVTOOLS_CMDS outside"),
    ("dev.txt named outside devtools.cpp", {"extra": {"kernel/x.cpp": 'const char* f() { return "dev.txt"; }\n'}},
     "names dev.txt outside"),
    ("non-literal tool name", {"extra": {"kernel/x.cpp": 'namespace sr{namespace kernel{bool devToolAllowed(const char*);}}\n'
                                                       'bool f(const char* n) { return sr::kernel::devToolAllowed(n); }\n'}},
     "non-literal argument"),
    ("tool outside the closed set", {"extra": {"kernel/x.cpp": 'namespace sr{namespace kernel{bool devToolAllowed(const char*);}}\n'
                                                              'bool f() { return sr::kernel::devToolAllowed("godmode"); }\n'}},
     "names no tool of the closed set"),
    ("devtool gate ORed", {"extra": {"kernel/x.cpp": 'namespace sr{namespace kernel{bool devToolAllowed(const char*);}}\n'
                                                    'bool f(bool x) { return sr::kernel::devToolAllowed("levelskip") || x; }\n'}},
     "ORed with another condition"),
    ("devtool gate ORed (left)", {"extra": {"kernel/x.cpp": 'namespace sr{namespace kernel{bool devToolAllowed(const char*);}}\n'
                                                           'bool f(bool x) { return x || devToolAllowed("levelskip"); }\n'}},
     "ORed with another condition"),
    ("JSON row missing", {"json_edit": drop_row("freecam")}, "closed set mismatch"),
    ("JSON class differs", {"json_edit": set_class("menuinput", "observe")}, "closed set mismatch"),
    ("'all' added to the closed set", {"edits": {"kernel/devtools.cpp": ('{"hud", "observe"},', '{"hud", "observe"}, {"all", "devtool"},')}},
     "'all' is not a tool"),
    ("default-ON idiom in the gate", {"edits": {"kernel/devtools.cpp": ("void ensureInit() {", "bool defOn(const char* e) { return !e || e[0] != '0'; }\nvoid ensureInit() {")}},
     "default-ON idiom"),
    # iter-463 review: realistic default-ON edits the `!e ||` check alone let through (5/5)
    ("default-ON: dev.txt-less source pushes a tool", {"edits": {"kernel/devtools.cpp": ('    d.source = "none";\n',
                                                                                        '    d.source = "none";\n    names.push_back("levelskip");\n')}},
     "names tool 'levelskip' outside"),
    ("default-ON: mask seeded by tool name", {"edits": {"kernel/devtools.cpp": ("  uint32_t mask = 0;\n",
                                                                               '  uint32_t mask = 1u << static_cast<uint32_t>(toolIndex("levelskip"));\n')}},
     "names tool 'levelskip' outside"),
    ("default-ON: mask seeded by a number", {"edits": {"kernel/devtools.cpp": ("  uint32_t mask = 0;\n", "  uint32_t mask = 0x8;\n")}},
     "a mask seeded with a non-zero literal"),
    ("default-ON: hard-coded tool bit", {"edits": {"kernel/devtools.cpp": ("  uint32_t mask = 0;\n", "  uint32_t mask = 0;\n  mask |= 1u << 3;\n")}},
     "a hard-coded tool bit"),
    ("default-ON: applyMask(literal)", {"edits": {"kernel/devtools.cpp": ("  applyMask(mask);\n", "  applyMask(mask ? mask : 8);\n  applyMask(8);\n")}},
     "the mask set to a non-zero literal"),
    ("default-ON: kTools[literal]", {"edits": {"kernel/devtools.cpp": ("  DevToolsDecision d;\n",
                                                                      "  DevToolsDecision d;\n  if (!srDevtools) srDevtools = kTools[3].name;\n")}},
     "indexes kTools[] by a literal"),
    ("default-ON: env fallback literal", {"edits": {"kernel/devtools.cpp": ("  DevToolsDecision d;\n",
                                                                           '  DevToolsDecision d;\n  if (!srDevtools) srDevtools = "levelskip";\n')}},
     "names tool 'levelskip' outside"),
    ("default-ON: devToolAllowed short-circuit", {"edits": {"kernel/devtools.cpp": ("  return i >= 0 && (g_mask.load",
                                                                                   "  return true || (i >= 0 && (g_mask.load")}},
     "can answer yes without the mask"),
    ("default-ON: tick hooks forced on", {"edits": {"kernel/devtools.cpp": ("  g_hook.store(hook);\n", "  g_hook.store(true);\n")}},
     "the tick hooks forced on"),
    ("default-ON: test seam called from the app", {"extra": {"app/x.cpp": "namespace sr{namespace kernel{void devToolsResetForTest(unsigned);}}\n"
                                                                          "void boot() { sr::kernel::devToolsResetForTest(8u); }\n"}},
     "calls the test seam devToolsResetForTest"),
    ("gate consults forcingAllowed", {"edits": {"kernel/devtools.cpp": ("void devToolsInitEager() { ensureInit(); }",
                                                                       "bool forcingAllowed();\nvoid devToolsInitEager() { ensureInit(); (void)forcingAllowed(); }")}},
     "consults forcingAllowed"),
    ("forcingAllowed consults the gate", {"extra": {"kernel/guest_context.cpp": 'namespace sr{namespace kernel{bool devToolAllowed(const char*);\n'
                                                                                 'bool forcingAllowed() { return devToolAllowed("levelskip"); }}}\n'}},
     "forcingAllowed() consults the dev-tool gate"),
    ("gate named in generated code", {"extra": {"guest/ppc/ppc_recomp.0.cpp": "void x() { devToolAllowed(\"levelskip\"); }\n"}},
     "never enters generated code"),
    ("observe tool writes guest memory", {"extra": {"kernel/x.cpp": SITE_OK.replace('"levelskip"', '"objstate"').replace("toolWrite", "obsWrite").replace("sub_82000050", "sub_82000054")}},
     "observe-class tool (objstate) writes"),
]


def run():
    fails = 0
    tmp = tempfile.mkdtemp(prefix="audit_devtools_")
    try:
        for name, kw, want in CASES:
            root = build(tmp, **kw)
            v, sites = verdict(root)
            if want is None:
                dts = [s for s in sites if s[0] == "DEVTOOL"]
                ok = not v and len(dts) >= 2      # site.cpp's write + the real liveWrite
            else:
                ok = any(want in x for x in v)
            fails += not ok
            print("  %s %-44s %s" % ("ok  " if ok else "FAIL", name, "" if ok else v[:3]))
        # the live tree
        parsed, sites = af.collect(ROOT)
        v = af.devtool_report(parsed, ROOT, sites, quiet=True)
        ok = not v and any(s[0] == "DEVTOOL" and "SR_DEVTOOLS:levelskip" in s[5] for s in sites)
        fails += not ok
        print("  %s %-44s %s" % ("ok  " if ok else "FAIL", "live tree: clean, liveWrite is DEVTOOL/levelskip", v[:3]))
        # docs/HOST_THREADS.json: the devtool class has ZERO rows (design 2.2)
        reg = json.load(open(os.path.join(ROOT, "docs", "HOST_THREADS.json"), encoding="utf-8"))
        ok = reg.get("devtool") == [] and not [e for e in reg.get("allow", []) if e.get("class") == "devtool"]
        fails += not ok
        print("  %s %-44s" % ("ok  " if ok else "FAIL", "HOST_THREADS.json devtool class: zero rows"))
        # ...and a planted devtool row fails scripts/audit_host_threads.py
        import io
        import audit_host_threads as aht
        for rows, want in (([], 0), ([{"file": "kernel/x.cpp", "entry": "pollThread"}], 1)):
            root = build(tmp)
            with open(os.path.join(root, "docs", "HOST_THREADS.json"), "w", encoding="utf-8") as f:
                json.dump({"allow": [], "declared": [], "open_forcing": [], "devtool": rows}, f)
            rc = aht.run(root, "--summary", out=io.StringIO())
            ok = rc == want
            fails += not ok
            print("  %s %-44s rc=%s" % ("ok  " if ok else "FAIL", "host-thread devtool rows %d -> rc %d" % (len(rows), want), rc))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("RESULT: %d failed" % fails)
    return fails


if __name__ == "__main__":
    sys.exit(1 if run() else 0)
