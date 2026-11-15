"""Offline parity test for the CHEATS direct call (charter 2a, owner decision 2026-10-06).

The CHEATS page (kernel/cheats_menu.cpp) must make the SAME calls with the SAME arguments, under the
SAME condition, as the game's own cheat matcher CheatMgr_FeedPadButton_sub_82237360 does when a pad
code completes. This test reads the matcher's recompiled body (guest/ppc, generated from the retail
XEX) and the page's source, and fails on any difference:

  matcher (guest/ppc)                                   page (kernel/cheats_menu.cpp)
  this = *0x8296DF30 (caller sub_8214E9D8)              G_CHEAT_MGR = 0x8296DF30
  flow = sub_821F68E8(*0x8296DF74)                      call(sub_821F68E8, rd(G_FLOW_MGR))
  key  = sub_826CD320("objectiveActivated" 0x8200A07C)  S_OBJECTIVE_ACTIVATED = 0x8200A07C
  rec  = sub_82194E48(*(flow+104), key); +4 == 0         flowValue(): das = rd(flow + 104u), rec + 4
  sub_82224EE0(*0x8296DF5C, 6) & 0xFF != 0              call(sub_82224EE0, rd(G_OBJ_WORLD), 6u) & 0xFF
  sub_82236840(this, index, 1)                          call(sub_82236840, mgr, uint32_t(idx), 1u)

The live half (the cheat state both paths leave) is archive/v1_charswitch/cheat_parity.py.

    python tools/test_cheats_direct_call.py      (exit 0 = parity)
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def body(fn):
    for path in sorted(glob.glob(os.path.join(ROOT, "guest", "ppc", "ppc_recomp.*.cpp"))):
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        at = text.find("PPC_FUNC_IMPL(__imp__%s)" % fn)
        if at >= 0:
            end = text.find("PPC_FUNC_IMPL(", at + 10)
            code = text[at:end if end > 0 else len(text)]
            return [ln.strip() for ln in code.splitlines() if ln.strip() and not ln.strip().startswith("//")]
    sys.exit("FAIL: %s not found in guest/ppc (generate the recomp first)" % fn)


def va(lines, i, reg):
    """The constant a register holds at line i: lis (s64 = -N) then addi (s64 = s64 + k)."""
    val = None
    for ln in lines[:i]:
        m = re.match(r"%s\.s64 = (-?\d+);" % re.escape(reg), ln)
        if m:
            val = int(m.group(1))
        m = re.match(r"%s\.s64 = %s\.s64 \+ (-?\d+);" % (re.escape(reg), re.escape(reg)), ln)
        if m and val is not None:
            val += int(m.group(1))
    return None if val is None else val & 0xFFFFFFFF


fails = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails.append(what)


# --- the matcher, as the retail code has it ---
m = body("sub_82237360")
calls = [(i, ln) for i, ln in enumerate(m) if re.match(r"sub_[0-9A-F]{8}\(ctx, base\);", ln)]
names = [ln.split("(")[0] for _, ln in calls]
want_order = ["sub_821F68E8", "sub_826CD320", "sub_82194E48", "sub_82224EE0", "sub_82236840", "sub_82230028"]
it = iter(names)
check(all(any(n == w for n in it) for w in want_order), "matcher calls, in order: " + " -> ".join(want_order))

i_key = next(i for i, ln in calls if ln.startswith("sub_826CD320"))
check(va(m, i_key, "r11") == 0x8200A07C, "matcher: sub_826CD320(0x8200A07C 'objectiveActivated')")
i_obj = next(i for i, ln in calls if ln.startswith("sub_82224EE0"))
window = m[i_obj - 4:i_obj]
check("ctx.r4.s64 = 6;" in window, "matcher: sub_82224EE0(..., 6)")
check(any("ctx.r3.u64 = PPC_LOAD_U32(r11.u32 + -8356);" == ln for ln in window) and
      (va(m, i_obj, "r11") or 0) + (-8356 & 0xFFFFFFFF) & 0xFFFFFFFF == 0x8296DF5C,
      "matcher: sub_82224EE0(*0x8296DF5C, ...)")
i_eff = next(i for i, ln in calls if ln.startswith("sub_82236840"))
window = m[i_eff - 4:i_eff]
check("ctx.r5.s64 = 1;" in window and "ctx.r3.u64 = r31.u64;" in window and "ctx.r4.u64 = r29.u64;" in window,
      "matcher: sub_82236840(this r31, index r29, 1)")
check(any("r30.u64 = PPC_LOAD_U32(ctx.r3.u32 + 104);" == ln for ln in m[:i_key]),
      "matcher: the DAS is *(flow + 104)")

# --- the matcher's caller hands it the cheat manager *0x8296DF30 ---
c = body("sub_8214E9D8")
i_call = next(i for i, ln in enumerate(c) if ln == "sub_82237360(ctx, base);")
ld = next((ln for ln in reversed(c[i_call - 5:i_call]) if ln.startswith("ctx.r3.u64 = PPC_LOAD_U32(")), "")
mm = re.match(r"ctx\.r3\.u64 = PPC_LOAD_U32\((r\d+)\.u32 \+ (-?\d+)\);", ld)
check(bool(mm) and ((va(c, i_call, mm.group(1)) or 0) + int(mm.group(2))) & 0xFFFFFFFF == 0x8296DF30,
      "caller sub_8214E9D8: this = *0x8296DF30")

# --- the page ---
with open(os.path.join(ROOT, "kernel", "cheats_menu.cpp"), encoding="utf-8") as f:
    page = f.read()


def const(name):
    mm = re.search(r"\b%s = (0x[0-9A-Fa-f]+)u" % name, page)
    return int(mm.group(1), 16) if mm else None


check(const("G_CHEAT_MGR") == 0x8296DF30, "page: G_CHEAT_MGR = 0x8296DF30")
check(const("G_FLOW_MGR") == 0x8296DF74, "page: G_FLOW_MGR = 0x8296DF74")
check(const("G_OBJ_WORLD") == 0x8296DF5C, "page: G_OBJ_WORLD = 0x8296DF5C")
check(const("S_OBJECTIVE_ACTIVATED") == 0x8200A07C, "page: S_OBJECTIVE_ACTIVATED = 0x8200A07C")
check("call(ctx, base, sub_821F68E8, rd(G_FLOW_MGR))" in page, "page: sub_821F68E8(*G_FLOW_MGR)")
check("const uint32_t das = rd(flow + 104u);" in page and "call(ctx, base, sub_82194E48, das, key)" in page and
      "rd(rec + 4u)" in page, "page: sub_82194E48(*(flow + 104), key) -> +4")
check("call(ctx, base, sub_82224EE0, rd(G_OBJ_WORLD), 6u) & 0xFFu" in page, "page: sub_82224EE0(*G_OBJ_WORLD, 6) & 0xFF")
check("if (*objActive != 0) return false;" in page, "page: objectiveActivated must be 0 (a missing one refuses)")
check("call(ctx, base, sub_82236840, mgr, uint32_t(idx), 1u)" in page, "page: sub_82236840(mgr, index, 1)")
check(page.count("call(ctx, base, sub_82236840") == 1, "page: sub_82236840 called from one place only")
check("cheatsVirtualPadButtons" not in page, "page: no virtual pad presses")

print("PARITY (static)", "OK" if not fails else "FAILED: %d" % len(fails))
sys.exit(1 if fails else 0)
