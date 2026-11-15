#!/usr/bin/env python3
"""Symbolizer for the perf tools (iter-464 perf lane, docs/PERFORMANCE.md).

Symbolizer(modules) where modules = {index: (path, base, size)} as written in the `mod`
lines of a host profile; then resolve([(index, rva), ...]) and sym(index, rva).

- The game exe: llvm-symbolizer (batch, one process) over the exe + its PDB. The PDB's
  public symbols name every recompiled guest function (__imp__sub_XXXXXXXX / sub_XXXXXXXX:
  the host symbol IS the guest VA) and every platform function. (System32 DbgHelp does
  not load this lld PDB; llvm-symbolizer does.)
- Any other module (ntdll, KernelBase, d3d12, the GPU driver): its PE export table,
  nearest export at or below the RVA ("ntdll!NtWaitForSingleObject"). No symbol server.
"""
from __future__ import annotations

import bisect
import os
import struct
import subprocess

LLVM_SYMBOLIZER = os.environ.get("LLVM_SYMBOLIZER", r"C:\Program Files\LLVM\bin\llvm-symbolizer.exe")


def pe_exports(path: str) -> tuple[list[int], list[str]]:
    """(sorted rvas, names) of a PE file's export table; ([], []) when unreadable."""
    try:
        data = open(path, "rb").read()
    except OSError:
        return [], []
    try:
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            return [], []
        nsect = struct.unpack_from("<H", data, pe + 6)[0]
        optsz = struct.unpack_from("<H", data, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", data, opt)[0]
        ddoff = opt + (112 if magic == 0x20B else 96)
        exp_rva, exp_size = struct.unpack_from("<II", data, ddoff)
        sects = []
        so = opt + optsz
        for i in range(nsect):
            vsize, va, rawsz, rawptr = struct.unpack_from("<IIII", data, so + i * 40 + 8)
            sects.append((va, max(vsize, rawsz), rawptr))

        def off(rva):
            for va, sz, raw in sects:
                if va <= rva < va + sz:
                    return raw + (rva - va)
            return None

        eo = off(exp_rva)
        if eo is None:
            return [], []
        (nfuncs, nnames, afuncs, anames, aords) = struct.unpack_from("<IIIII", data, eo + 20)
        fo, no, oo = off(afuncs), off(anames), off(aords)
        funcs = struct.unpack_from("<%dI" % nfuncs, data, fo)
        out = {}
        for i in range(nnames):
            nrva = struct.unpack_from("<I", data, no + i * 4)[0]
            ordi = struct.unpack_from("<H", data, oo + i * 2)[0]
            frva = funcs[ordi]
            if exp_rva <= frva < exp_rva + exp_size:
                continue  # forwarder
            p = off(nrva)
            end = data.index(b"\0", p)
            name = data[p:end].decode("ascii", "replace")
            # Prefer Nt* over Zw* aliases and the shortest name for duplicates.
            if frva not in out or (out[frva].startswith("Zw") and name.startswith("Nt")):
                out[frva] = name
        rvas = sorted(out)
        return rvas, [out[r] for r in rvas]
    except (struct.error, ValueError, IndexError):
        return [], []


class Symbolizer:
    def __init__(self, modules: dict[int, tuple[str, int, int]], exe_override: str | None = None):
        self.modules = modules
        self.exe_override = exe_override
        self.cache: dict[tuple[int, int], str] = {}
        self.exports: dict[int, tuple[list[int], list[str]]] = {}

    def _is_exe(self, idx: int) -> bool:
        m = self.modules.get(idx)
        return bool(m) and m[0].lower().endswith(".exe")

    def _exe_path(self, idx: int) -> str:
        p = self.modules[idx][0]
        if self.exe_override and p.lower().endswith("supermanreturns.exe"):
            return self.exe_override
        return p

    def resolve(self, keys) -> None:
        """Batch-resolve (index, rva) pairs into the cache."""
        todo = [k for k in set(keys) if k not in self.cache]
        by_exe: dict[str, list[tuple[int, int]]] = {}
        for idx, rva in todo:
            if idx not in self.modules:
                self.cache[(idx, rva)] = "?unknown!%x" % rva
            elif self._is_exe(idx):
                by_exe.setdefault(self._exe_path(idx), []).append((idx, rva))
            else:
                self.cache[(idx, rva)] = self._export_sym(idx, rva)
        for exe, items in by_exe.items():
            names = self._llvm(exe, [rva for _, rva in items])
            mod = os.path.basename(self.modules[items[0][0]][0]).rsplit(".", 1)[0]
            for (idx, rva), nm in zip(items, names):
                self.cache[(idx, rva)] = "%s!%s" % (mod, nm) if nm else "%s!+%x" % (mod, rva)

    def _llvm(self, exe: str, rvas: list[int]) -> list[str]:
        if not os.path.exists(exe) or not os.path.exists(LLVM_SYMBOLIZER):
            return [""] * len(rvas)
        # The image base the linker chose (llvm-symbolizer takes VAs in that space).
        base = 0x140000000
        try:
            data = open(exe, "rb").read(4096)
            pe = struct.unpack_from("<I", data, 0x3C)[0]
            base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
        except (OSError, struct.error):
            pass
        inp = "".join("0x%x\n" % (base + r) for r in rvas)
        p = subprocess.run([LLVM_SYMBOLIZER, "--obj=" + exe, "--functions=linkage", "--no-inlines",
                            "--output-style=GNU"], input=inp, capture_output=True, text=True)
        lines = p.stdout.splitlines()
        # GNU style: two lines per address (function, file:line).
        out = []
        for i in range(len(rvas)):
            fn = lines[2 * i].strip() if 2 * i < len(lines) else ""
            out.append("" if fn in ("??", "") else fn)
        return out

    def _export_sym(self, idx: int, rva: int) -> str:
        path, _, _ = self.modules[idx]
        mod = os.path.basename(path).rsplit(".", 1)[0]
        if idx not in self.exports:
            self.exports[idx] = pe_exports(path)
        rvas, names = self.exports[idx]
        i = bisect.bisect_right(rvas, rva) - 1
        if i < 0:
            return "%s!+%x" % (mod, rva)
        return "%s!%s" % (mod, names[i])

    def sym(self, idx: int, rva: int) -> str:
        k = (idx, rva)
        if k not in self.cache:
            self.resolve([k])
        return self.cache[k]


def short(name: str) -> str:
    """SupermanReturns!sub_82110840 -> sub_82110840; other modules keep their prefix.
    __imp__sub_X stays as is (iter-465): when a kernel override defines sub_X, __imp__sub_X is
    the recompiled GUEST body and sub_X the HOOK - merging them billed guest work to the hook."""
    mod, _, fn = name.partition("!")
    if mod.lower() == "supermanreturns":
        mod = ""
    paren = fn.find("(")
    if paren > 0:
        fn = fn[:paren]
    return (mod + "!" + fn) if mod else fn
