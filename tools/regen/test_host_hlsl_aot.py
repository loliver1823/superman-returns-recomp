#!/usr/bin/env python3
"""Tests for tools/regen/host_hlsl_aot.py (step 4 item 7a): the manifest against the code's compile call sites, the
literal extraction, and the key against gfx/host_hlsl.cpp's (tools/difftest/host_hlsl_test.cpp asserts the same three
reference keys on the C++ side).

    python -m unittest tools/regen/test_host_hlsl_aot.py
"""
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import host_hlsl_aot as aot  # noqa: E402


def read(path):
    return open(os.path.join(REPO, path), "rb").read().decode("utf-8")


def call_sites():
    """{unit name: {(entry, target)}} found in the code's compile calls (the patterns each helper is called with)."""
    found = {}

    def add(unit, entry, target):
        found.setdefault(unit, set()).add((entry, target))

    for m in re.finditer(r'pm4CompileXfer\("(\w+)", "(\w+)"\)', read(aot.FR)):
        add("pm4_xfer.hlsl", *m.groups())
    em = read(aot.EM)
    for m in re.finditer(r'edramCompile\("(\w+)", "(\w+)"\)', em):
        add("pm4_edram.hlsl", *m.groups())
    for m in re.finditer(r'pm4CompileHlsl\(\w+, [^,]+, "([\w.-]+)", "(\w+)", "(\w+)"\)', em):
        add(m.group(1), m.group(2), m.group(3))
    for m in re.finditer(r'shmCompile\("(\w+)", "(\w+)"\)', read(aot.SH)):
        add("pm4_shm_present.hlsl", *m.groups())
    for m in re.finditer(r'overlayCompile\("(\w+)", "(\w+)"\)', read(aot.OV)):
        add("host_overlay.hlsl", *m.groups())
    for m in re.finditer(r'hostHlslCompile\(kGeoShaderSrc, [^,]+, "([\w.]+)", nullptr,\s*"(\w+)", "(\w+)"', read(aot.BE)):
        add(m.group(1), m.group(2), m.group(3))
    pc = read(aot.PC)
    for m in re.finditer(r'chainCompile\(\w+, [^,]+, "([\w.]+)", "(\w+)",\s*"(\w+)"\)', pc):
        add(m.group(1), m.group(2), m.group(3))
    for m in re.finditer(r'pm4ChainPso\("(\w+)", \w+, [^,]+, "(\w+)"', pc):
        add(m.group(1), m.group(2), "ps_5_1")   # pm4ChainPso compiles its PS as ps_5_1 under the pass name
    for m in re.finditer(r'\bcompile\("(\w+)", "(\w+)"\)', read(aot.CV)):
        add("costume_viewer.hlsl", *m.groups())
    return found


class Manifest(unittest.TestCase):
    def test_every_call_site_variant_is_in_the_manifest(self):
        units = {u[0]: set(u[2]) for u in aot.UNITS}
        sites = call_sites()
        self.assertGreaterEqual(sum(len(v) for v in sites.values()), 25, sites)   # the scan itself found the calls
        for unit, pairs in sites.items():
            self.assertIn(unit, units, unit)
            self.assertLessEqual(pairs, units[unit], "%s: compiled at runtime but not ahead of time: %s" % (
                unit, sorted(pairs - units[unit])))

    def test_no_unrouted_d3dcompile_left_in_gfx(self):
        for fn in sorted(os.listdir(os.path.join(REPO, "gfx"))):
            if fn.endswith(".cpp") and fn != "host_hlsl.cpp":
                self.assertNotRegex(read("gfx/" + fn), r"(?<![\w:])D3DCompile\(", fn)

    def test_every_literal_extracts_once(self):
        vs = aot.variants(REPO)
        self.assertEqual(len(vs), sum(len(u[2]) * len(u[3]) for u in aot.UNITS))
        for v in vs:
            self.assertGreater(len(v["src"]), 100, v["label"])
            self.assertNotIn(b"\r", v["src"], v["label"])
        self.assertEqual(len({v["key"] for v in vs}), len(vs), "every variant has its own key")

    def test_oracle_units_are_the_oracle_files(self):
        for name, parts, _e, _d, _f, oracle in aot.UNITS:
            in_oracle_file = any(p in (aot.EM, aot.BE) and n != "kPm4PackHlsl" for p, n in parts)
            self.assertEqual(oracle, in_oracle_file, name)


    def test_cmake_edges_name_exactly_the_files_each_half_reads(self):
        """cmake/host_hlsl.cmake's two source lists are each half's files (its edge's DEPENDS and --file arguments),
        and no COMMAND names the source root (a folder holding the build tree: the composition audit reads it as
        naming every file under it)."""
        cm = read("cmake/host_hlsl.cmake")
        for half, var in (("ship", "SR_HOST_HLSL_SHIP_SRC"), ("oracle", "SR_HOST_HLSL_ORACLE_SRC")):
            m = re.search(r"set\(%s\s+([^)]*)\)" % var, cm)
            self.assertIsNotNone(m, var)
            want = {p for name, parts, _e, _d, _f, oracle in aot.UNITS if oracle == (half == "oracle") for p, _n in parts}
            self.assertEqual(set(m.group(1).split()), want, var)
        self.assertNotIn("--root", cm)
        # the compiler pin: both edges pass it and depend on it
        self.assertEqual(cm.count('--pin "${SR_HOST_HLSL_PIN}"'), 2)
        self.assertEqual(cm.count("tools/regen/d3dcompiler_47.pin.json ${SR_HOST_HLSL_"), 2)

    def test_compiler_pin(self):
        import json, tempfile
        d = tempfile.mkdtemp()
        p = os.path.join(d, "pin.json")
        ident = {"dll": "d3dcompiler_47.dll", "sha256": "ab" * 32, "size": 10, "version": "1.2.3.4"}
        json.dump({"sha256": "ab" * 32, "size": 10}, open(p, "w"))
        aot.check_pin(ident, p)   # the pinned one passes
        json.dump({"sha256": "cd" * 32, "size": 10}, open(p, "w"))
        with self.assertRaises(SystemExit):
            aot.check_pin(ident, p)
        json.dump({"sha256": "ab" * 32, "size": 11}, open(p, "w"))
        with self.assertRaises(SystemExit):
            aot.check_pin(ident, p)
        pin = json.load(open(os.path.join(REPO, "tools", "regen", "d3dcompiler_47.pin.json"), encoding="utf-8"))
        self.assertEqual(len(pin["sha256"]), 64)
        self.assertGreater(pin["size"], 0)

    def test_given_files_only(self):
        import tempfile
        d = tempfile.mkdtemp()
        with open(os.path.join(d, "x.cpp"), "wb") as f:
            f.write(b'const char kA[] = R"(a)";\n')
        aot.FILES.clear()
        aot.FILES["gfx/x.cpp"] = os.path.join(d, "x.cpp")
        try:
            self.assertEqual(aot.extract("", "gfx/x.cpp", "kA"), b"a")
            with self.assertRaises(SystemExit):   # a file the edge does not name is not read
                aot.extract(d, "x.cpp", "kA")
        finally:
            aot.FILES.clear()


class Key(unittest.TestCase):
    def test_reference_keys(self):   # the same three values as tools/difftest/host_hlsl_test.cpp
        self.assertEqual(aot.key(b"float4 main() : SV_Target { return 1; }\n", "k.hlsl", [], "main", "ps_5_1", 1 << 15),
                         0x0032BB47F54857F6)
        self.assertEqual(aot.key(b"x", "n", [("SR_HALF_HEXCMP", "1")], "e", "t", 0, 0), 0x1D8352E6F4E22EB7)
        self.assertEqual(aot.key(b"", "", [], "", "", 0, 0), 0x6E3E1B0C8ECA9DEF)

    def test_extract_reads_line_ends_as_lf(self):
        import tempfile
        d = tempfile.mkdtemp()
        with open(os.path.join(d, "x.cpp"), "wb") as f:
            f.write(b'const char kA[] = R"HLSL(\r\nfloat a;\r\n)HLSL";\r\nconst char* kB = R"hlsl(b)hlsl";\r\n')
        self.assertEqual(aot.extract(d, "x.cpp", "kA"), b"\nfloat a;\n")
        self.assertEqual(aot.extract(d, "x.cpp", "kB"), b"b")
        with self.assertRaises(SystemExit):
            aot.extract(d, "x.cpp", "kC")


if __name__ == "__main__":
    unittest.main()
