#!/usr/bin/env python3
"""NATIVE ROADMAP N0.3 (docs/NATIVE_ROADMAP.md §4.1): generate the observe shims.

Reads
  data/native/xdk_entry_classes.tsv   213 external XDK D3D entry points (N0.1)
  data/native/xdk_table_setters.tsv   146 setters reached only through device tables
  INTERNALS below                     device internals that split a bracket
                                      (flush, segment switch), wait, write the
                                      ring, or run the pump
and writes kernel/native_brackets.gen.cpp:
  * kEntries[] - every row, sorted by VA (index = the scope id at runtime);
  * one strong-def shim per row that has NO existing override, as one line
        SR_NATIVE_SHIM(sub_X, k)
    of a macro that defines sub_X { XdkScope(k, ctx); __imp__sub_X(ctx, base); }
    (observe: one unconditional delegate, nothing written to the guest).

Rows that ALREADY have a strong-def elsewhere (kernel/*.cpp, guest/*.cpp, the
hand stubs) cannot get a second one. Their existing override must carry
`SR_NATIVE_SCOPE(0xVA);` as its first statement; this script FAILS (exit 1)
listing any that do not, so the instrumentation can never silently miss an
entry. Rows whose VA has no recompiled body (no `PPC_FUNC_IMPL(__imp__sub_X)`
in guest/ppc) are skipped and reported (needs a local guest/ppc).

After running: python scripts/gen_override_manifest.py && python scripts/audit_forcing.py
"""
import csv
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENTRY_TSV = ROOT / "data/native/xdk_entry_classes.tsv"
TABLE_TSV = ROOT / "data/native/xdk_table_setters.tsv"
OUT = ROOT / "kernel/native_brackets.gen.cpp"
PPC_DIR = ROOT / "guest/ppc"
OVERRIDE_SCAN = [ROOT / "kernel", ROOT / "guest"]
HAND = [ROOT / "tools/regen/hand/ppc_missing_recomp_stubs.cpp"]

# VA -> (kind, name). Kinds match sr::native::Kind.
INTERNALS = {
    0x820FD8C0: ("kFlush", "GpuCommandBufferFlush_sub_820FD8C0"),
    0x820FCF90: ("kSwitch", "GpuCmdBufBeginNewSegment_sub_820FCF90"),
    0x820FD9A8: ("kWait", "GpuBlockOnFence_sub_820FD9A8"),
    0x820FCB30: ("kWait", "GpuRingWaitForSpace_sub_820FCB30"),
    0x820FD150: ("kWait", "GpuRingWaitReadPtrWrap_sub_820FD150"),
    0x821102B8: ("kWait", "GpuWaitTilesComplete_sub_821102B8"),
    0x820FD218: ("kRingWrite", "GpuRingUpdateWritePointer_sub_820FD218"),
    0x82110840: ("kInternal", "GpuCommandBufferReplayDriver_sub_82110840"),
    # The PM4 census's stamp lifetime (kernel/native_brackets.hpp): the XDK's block allocator hands memory out again
    # (its scope hooks erase the stale stamps of the block it returns).
    0x820FCDF0: ("kInternal", "GpuCmdBufAllocate_sub_820FCDF0"),
}
PRESENT = {0x82112050, 0x82112750}  # Swap, BlitAndSwap
CLS = {
    "cpu-only-keep": "kClsKeep",
    "emitter-replace": "kClsEmitter",
    "resource-sync-replace": "kClsResSync",
    "dead": "kClsDead",
}

DEF_RE = re.compile(
    r"^(?:void\s+(sub_[0-9A-Fa-f]{8})\s*\(\s*PPCContext|PPC_FUNC\((sub_[0-9A-Fa-f]{8})\))", re.M)


def read_rows():
    rows = {}
    for r in csv.DictReader(open(ENTRY_TSV, encoding="utf-8"), delimiter="\t"):
        va = int(r["va"], 16)
        rows[va] = {"name": r["name"], "kind": "kEntry", "cls": CLS[r["class"]]}
    for r in csv.DictReader(open(TABLE_TSV, encoding="utf-8"), delimiter="\t"):
        va = int(r["va"], 16)
        if va in rows:
            continue
        rows[va] = {"name": r["name"], "kind": "kTable", "cls": CLS[r["class"]]}
    for va, (kind, name) in INTERNALS.items():
        if va in rows:
            rows[va]["kind"] = kind
        else:
            rows[va] = {"name": name, "kind": kind, "cls": "kClsInternal"}
    return rows


def recompiled_bodies():
    syms = set()
    for f in PPC_DIR.glob("ppc_recomp.*.cpp"):
        for m in re.finditer(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)", f.read_text(errors="replace")):
            syms.add(int(m.group(1), 16))
    return syms


def existing_overrides():
    """sym -> (file, body text) for every strong-def outside the generated file.

    Uses the override registry's own scanner pieces (scripts/gen_override_manifest.py:
    literal definitions AND macro-generated ones such as SR_PUMP_PROBE(sub_X)), so
    this generator and docs/OVERRIDES.json can never disagree about what exists.
    """
    sys.path.insert(0, str(ROOT / "scripts"))
    import gen_override_manifest as gom  # noqa: E402

    out = {}
    files = []
    for d in OVERRIDE_SCAN:
        files += [p for p in d.glob("*.cpp")]
    files += [p for p in HAND if p.exists()]
    for f in sorted(files):
        if f.resolve() == OUT.resolve():
            continue
        text = f.read_text(encoding="utf-8", errors="replace")
        rel = f.relative_to(ROOT).as_posix()
        for m in gom.DEF_RE.finditer(text):
            sym = m.group(1) or m.group(2)
            if not gom.is_definition(text, m.end()):
                continue
            out[sym] = (rel, gom.extract_body(text, m.end()))
        macros = gom.collect_macros(text)
        for m in gom.MACRO_CALL_RE.finditer(text):
            macro, sym = m.group(1), m.group(2)
            if macro not in macros or sym in out:
                continue
            param, mbody = macros[macro]
            out[sym] = (rel + " (macro " + macro + ")", gom.macro_body_for(sym, param, mbody))
    return out


def main():
    rows = read_rows()
    bodies = recompiled_bodies()
    if not bodies:
        print("gen_native_brackets: guest/ppc has no recompiled bodies here - run in a tree with guest/ppc")
        return 2
    existing = existing_overrides()
    vas = sorted(rows)
    missing_body = [va for va in vas if va not in bodies]
    missing_scope = []
    shims = []
    for idx, va in enumerate(vas):
        sym = "sub_%08X" % va
        if va in missing_body:
            continue
        if sym in existing:
            f, body = existing[sym]
            if not re.search(r"SR_NATIVE_SCOPE\(\s*0x%08X\s*\)|SR_NATIVE_SCOPE_SYM\(\s*%s\s*\)"
                             % (va, sym), body, re.I):
                missing_scope.append((sym, f))
            continue
        shims.append((idx, va, sym))

    lines = []
    w = lines.append
    w("// GENERATED by scripts/gen_native_brackets.py - do not edit.")
    w("// NATIVE ROADMAP N0.3 / N0.0: observe shims on the XDK D3D entry points (")
    w("// data/native/xdk_entry_classes.tsv), the table-only setters")
    w("// (data/native/xdk_table_setters.tsv) and the device internals that split a")
    w("// bracket, wait, write the ring or run the pump. Each shim opens an")
    w("// sr::native::XdkScope (a no-op unless SR_NATIVE_FRAMESPLIT=1 or")
    w("// SR_NATIVE_BRACKETS=1) and delegates to __imp__ exactly once.")
    w("// %d rows; %d generated shims; %d rows instrumented inside existing overrides." %
      (len(vas), len(shims), len(vas) - len(shims) - len(missing_body)))
    w("")
    w('#include "guest/ppc/ppc_recomp_shared.h"')
    w('#include "native_brackets.hpp"')
    w("")
    w("namespace sr::native {")
    w("const EntryInfo kEntries[] = {")
    for va in vas:
        r = rows[va]
        w('    {0x%08Xu, "%s", %s, %s, %d},' % (va, r["name"], r["kind"], r["cls"], 1 if va in PRESENT else 0))
    w("};")
    w("const int kEntryCount = %d;" % len(vas))
    w("}  // namespace sr::native")
    w("")
    # One macro, one line per shim: the override registry resolves macro-generated
    # strong-defs (gen_override_manifest.py collect_macros / MACRO_CALL_RE), and a
    # file of 329 literal function bodies made scripts/audit_forcing.py pathologically
    # slow (> 25 min vs 2.5 min for the tree without it).
    w("// SR_NATIVE_SHIM(sub_X, k): observe shim for kEntries[k]; delegates exactly once.")
    w("#define SR_NATIVE_SHIM(SYM, K) \\")
    w('  extern "C" void __imp__##SYM(PPCContext& __restrict ctx, uint8_t* base); \\')
    w("  void SYM(PPCContext& __restrict ctx, uint8_t* base) { \\")
    w("    ::sr::native::XdkScope _srNativeScope(K, ctx); \\")
    w("    __imp__##SYM(ctx, base); \\")
    w("  }")
    w("")
    for idx, va, sym in shims:
        w("SR_NATIVE_SHIM(%s, %d)  // %s" % (sym, idx, rows[va]["name"]))
    w("")
    w("#undef SR_NATIVE_SHIM")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print("gen_native_brackets: %d rows, %d shims -> %s" % (len(vas), len(shims), OUT.relative_to(ROOT)))
    if missing_body:
        print("  skipped (no recompiled body): %s" % ", ".join("%08X" % v for v in missing_body))
    if missing_scope:
        print("  FAIL: existing overrides without SR_NATIVE_SCOPE(0xVA) as first statement:")
        for sym, f in missing_scope:
            print("    %s  (%s)" % (sym, f))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
