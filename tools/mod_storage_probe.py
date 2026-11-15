#!/usr/bin/env python3
"""Bounded, read-only snapshot of the game's registered loose-file storages.

Use during a guarded live roll, or --golden sound_menu for the Xenia dump.
This measures the guest's consumed entries, not capped NtQueryDirectory logs.
The storage layout is documented in the owner's mod-system research, not published (research/modsystem/
storages_golden.py. No hooks, writes, guest calls, suspension or polling.
"""
import argparse
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / "srmods"))
from srmods import fnv1a64_upper


def snapshot(read, disc):
    def u32(va):
        return struct.unpack(">I", read(va, 4))[0]

    def vector(begin, end, stride, maximum):
        if begin > end or (end - begin) % stride or (end - begin) // stride > maximum:
            raise ValueError(f"invalid vector {begin:08x}..{end:08x}/{stride}")
        if begin != end and (not begin or begin % 4):
            raise ValueError("unaligned/null nonempty vector")
        return range(begin, end, stride)

    expected = {f"{fnv1a64_upper(p.stem):016x}": p.name
                for p in (disc / "DATA").iterdir()
                if p.is_file() and p.suffix.lower() == ".ast"}
    if not expected:
        raise ValueError("no disc AST files: refuse vacuous comparison")
    manager = u32(0x829A2430)
    if not manager or u32(manager) != 0x8208AE30:
        raise ValueError("resource storage manager not ready or wrong vtable")
    begin, end = u32(manager + 8), u32(manager + 12)
    result = {"manager": f"{manager:08x}", "expected_disc_ast": expected,
              "storages": []}
    for slot in vector(begin, end, 4, 2048):
        storage = u32(slot)
        if not storage or u32(storage) != 0x8208A078:
            continue
        entry_begin, entry_end = u32(storage + 264), u32(storage + 268)
        entries = []
        for entry in vector(entry_begin, entry_end, 40, 16384):
            raw = read(entry, 40)
            asset_id = raw[16:24].hex()
            entries.append({"id": asset_id, "disc_name": expected.get(asset_id),
                            "raw": raw.hex()})
        # A changing vector must not masquerade as a coherent census.
        if (entry_begin, entry_end) != (u32(storage + 264), u32(storage + 268)):
            raise ValueError("storage changed during observation; take a new snapshot")
        result["storages"].append({"address": f"{storage:08x}",
                                   "priority": u32(storage + 300),
                                   "count": len(entries), "entries": entries})
    if manager != u32(0x829A2430) or (begin, end) != (u32(manager + 8), u32(manager + 12)):
        raise ValueError("storage manager changed during observation")
    roots = [s for s in result["storages"] if s["priority"] == 50]
    found = {e["id"] for s in roots for e in s["entries"]}
    result["missing_disc_ast"] = [name for key, name in expected.items() if key not in found]
    result["all_disc_ast_registered"] = bool(roots) and not result["missing_disc_ast"]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--pid", type=int)
    source.add_argument("--golden")
    parser.add_argument("--disc", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    # Immutable evidence: never overwrite an earlier observation.
    if args.out.exists():
        parser.error("output already exists")
    if args.golden:
        import golden
        dump = golden.GoldenDump(args.golden)
        result = snapshot(dump.read, args.disc)
        result["source"] = {"golden": args.golden}
    else:
        import frida
        session = frida.attach(args.pid)
        try:
            script = session.create_script(r"""
                const sym = Module.findGlobalExportByName('g_debugGuestBase');
                if (!sym) throw new Error('not a supported recomp process');
                const base = sym.readPointer();
                if (base.isNull()) throw new Error('guest base not ready');
                rpc.exports = { read(va, size) {
                    if (!Number.isInteger(va) || va < 0 || size <= 0 ||
                        size > 65536 || va + size > 0x100000000)
                        throw new Error('invalid guest range');
                    // Volatile reads use the OS rather than risking a host AV;
                    // this harness makes access violations fatal before SEH.
                    return base.add(va).readVolatile(size);
                }};
            """)
            script.load()
            result = snapshot(script.exports_sync.read, args.disc)
            result["source"] = {"pid": args.pid, "mode": "read-only live snapshot"}
        finally:
            session.detach()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("x", encoding="utf-8") as handle:
        json.dump(result, handle, indent=2)
        handle.write("\n")
    print(json.dumps({"source": result["source"],
                      "storages": [{k: s[k] for k in ("address", "priority", "count")}
                                   for s in result["storages"]],
                      "missing": result["missing_disc_ast"], "out": str(args.out)}))


if __name__ == "__main__":
    main()
