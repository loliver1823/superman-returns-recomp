#!/usr/bin/env python3
"""host_hlsl_aot.py - compile the host's own HLSL at build time (step 4 item 7a, lane D, 2026-10-20).

The game compiled these shaders with D3DCompile at their first use - inside the first PM4 frame, on the CP (64-78 % of
the CP's boot -> intro hand-off second; gfx/host_hlsl.hpp). This tool runs at build time (cmake/host_hlsl.cmake):

  1. extracts each raw-string HLSL literal the code compiles from its C++ source (CRLF -> LF: clang reads a raw
     string's line ends as LF, checked 2026-10-20 with clang 22.1.6), and joins them the way the code does;
  2. compiles every (name, entry, target, defines, flags) variant the code passes, with the SAME D3DCompile the game
     calls (d3dcompiler_47.dll of System32, via ctypes; --dll overrides);
  3. writes two C++ units of byte arrays keyed exactly as gfx/host_hlsl.cpp hostHlslKey() keys a runtime call:
     --out (shipping shaders) and --out-oracle (the development build's oracle-only shaders: the EDRAM word model, the legacy
     geometry pass - compiled only into the SR_ORACLE=1 build).

A source the manifest below does not cover still works: the game compiles it at runtime and logs
`[HOST_HLSL] runtime compile`. tools/regen/test_host_hlsl_aot.py checks the manifest against the call sites.

    python tools/regen/host_hlsl_aot.py --root <repo> --out <cpp> --out-oracle <cpp> [--only ship|oracle] [--dll <dll>]
    python tools/regen/host_hlsl_aot.py --root <repo> --list          # the variants, no compile
"""
from __future__ import annotations

import argparse
import ctypes
import json
import hashlib
import os
import re
import sys

O3 = 1 << 15  # D3DCOMPILE_OPTIMIZATION_LEVEL3
HALF = [[], [("SR_HALF_HEXCMP", "1")]]  # pm4CompileHlsl: pm4HalfSignedOn() ? no macro : SR_HALF_HEXCMP=1
NONE = [[]]
FR, EM, SH = "gfx/d3d12_frame_render.cpp", "gfx/d3d12_edram_model.cpp", "gfx/d3d12_pm4_shmem.cpp"
OV, BE, PC, CV = "gfx/d3d12_overlay.cpp", "gfx/d3d12_backend.cpp", "gfx/d3d12_present_chain.cpp", \
    "gfx/costume_viewer_draw.cpp"

# (name the code passes to D3DCompile, [(file, literal)] joined in order, [(entry, target)], define variants, flags1,
#  oracle-only) - mirror of the call sites:
UNITS = [
    # pm4CompileXfer (d3d12_frame_render.cpp): kPm4PackHlsl + kPm4XferHlsl, via pm4CompileHlsl
    ("pm4_xfer.hlsl", [(FR, "kPm4PackHlsl"), (FR, "kPm4XferHlsl")],
     [("VSQuad", "vs_5_1"), ("PSColorDirect", "ps_5_1"), ("PSDepthFromDw", "ps_5_1"), ("PSStencilFromDw", "ps_5_1"),
      ("PSDwordDirect", "ps_5_1")], HALF, O3, False),
    # edramCompile (d3d12_edram_model.cpp, the oracle unit): kPm4PackHlsl + kPm4EdramStoreLoadHlsl, via pm4CompileHlsl
    ("pm4_edram.hlsl", [(FR, "kPm4PackHlsl"), (EM, "kPm4EdramStoreLoadHlsl")],
     [("CSStore", "cs_5_1"), ("PSColor", "ps_5_1"), ("PSDepth", "ps_5_1"), ("PSStencil", "ps_5_1")], HALF, O3, True),
    # the native depth probe (d3d12_edram_model.cpp), via pm4CompileHlsl
    ("nat-depth-probe", [(EM, "shader")], [("main", "cs_5_1")], HALF, O3, True),
    # shmCompile (d3d12_pm4_shmem.cpp)
    ("pm4_shm_present.hlsl", [(SH, "kShmPresentHlsl")],
     [("VSFull", "vs_5_1"), ("PSPresent", "ps_5_1"), ("PSFit", "ps_5_1"), ("PSPresentTex", "ps_5_1")], NONE, O3, False),
    # overlayCompile (d3d12_overlay.cpp)
    ("host_overlay.hlsl", [(OV, "kOverlayHlsl")], [("VSQuad", "vs_5_1"), ("PSCursor", "ps_5_1")], NONE, O3, False),
    # the legacy geometry pass (d3d12_backend.cpp, SR_ORACLE only): flags 0
    ("geo.hlsl", [(BE, "kGeoShaderSrc")], [("VSMain", "vs_5_0"), ("PSMain", "ps_5_0")], NONE, 0, True),
    # chainCompile (d3d12_present_chain.cpp): the VS, and pm4ChainPso's PS keyed by its pass name
    ("present_chain_vs.hlsl", [(PC, "kChainVsHlsl")], [("VSFull", "vs_5_1")], NONE, O3, False),
    ("fxaa_luma", [(PC, "kFxaaLumaHlsl")], [("PSLuma", "ps_5_1")], NONE, O3, False),
    ("colour", [(PC, "kColourHlsl")], [("PSColour", "ps_5_1")], NONE, O3, False),
    # costume_viewer_draw.cpp's compile lambda
    ("costume_viewer.hlsl", [(CV, "kHlsl")],
     [("VS", "vs_5_1"), ("PS", "ps_5_1"), ("VSBack", "vs_5_1"), ("PSBack", "ps_5_1")], NONE, O3, False),
]

LITERAL = r'\b%s\s*(?:\[\s*\])?\s*=\s*R"(\w*)\('


FILES = {}  # --file REL=ABS: where each source is read (the build edge names files, never the tree's root folder)


def extract(root, path, name):
    """The raw-string literal `name = R"D(...)D"` of `path`, its line ends as the compiler reads them (LF)."""
    if FILES and path not in FILES:   # the build edge reads only the files it names (and depends on)
        raise SystemExit("host_hlsl_aot: %s is read but not given with --file (add it to cmake/host_hlsl.cmake)" % path)
    where = FILES.get(path) or os.path.join(root, path)
    with open(where, "rb") as f:
        text = f.read().decode("utf-8")
    hits = list(re.finditer(LITERAL % re.escape(name), text))
    if len(hits) != 1:
        raise SystemExit("host_hlsl_aot: %s: %d raw-string literal(s) named %s (want exactly 1)" % (path, len(hits), name))
    m = hits[0]
    end = text.index(")%s\"" % m.group(1), m.end())
    return text[m.end():end].replace("\r\n", "\n").encode("utf-8")


def fnv(data, h):
    for b in data:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def key(src, name, defines, entry, target, flags1, flags2=0):
    """gfx/host_hlsl.cpp hostHlslKey(), byte for byte."""
    h = 1469598103934665603
    for part in ("SRHLSL1", name, entry, target, str(flags1), str(flags2)):
        h = fnv(part.encode("utf-8") + b"\0", h)
    for mn, mv in defines:
        h = fnv(mn.encode("utf-8") + b"=" + (mv or "").encode("utf-8") + b"\0", h)
    h = fnv(b"\1", h)
    return fnv(src, h)


def variants(root, only=None):
    """Every variant; only = "ship" / "oracle" reads and returns just that half (the release configuration's build
    edge must not read the oracle-only sources)."""
    out = []
    for name, parts, entries, defsets, flags1, oracle in UNITS:
        if only and oracle != (only == "oracle"):
            continue
        src = b"".join(extract(root, p, n) for p, n in parts)
        for entry, target in entries:
            for defs in defsets:
                label = "%s:%s:%s%s" % (name, entry, target, "".join(":%s=%s" % d for d in defs))
                out.append({"name": name, "src": src, "entry": entry, "target": target, "defines": defs,
                            "flags1": flags1, "oracle": oracle, "label": label,
                            "key": key(src, name, defs, entry, target, flags1)})
    return out


class Macro(ctypes.Structure):
    _fields_ = [("Name", ctypes.c_char_p), ("Definition", ctypes.c_char_p)]


def compiler(dll):
    path = dll or os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "d3dcompiler_47.dll")
    lib = ctypes.WinDLL(path)
    fn = lib.D3DCompile
    fn.restype = ctypes.c_long
    fn.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p,
                   ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p),
                   ctypes.POINTER(ctypes.c_void_p)]
    return fn, path


def file_version(path):
    """The DLL's VS_FIXEDFILEINFO file version "a.b.c.d" ("" when unreadable)."""
    try:
        ver = ctypes.WinDLL("version.dll")
        size = ver.GetFileVersionInfoSizeW(path, None)
        if not size:
            return ""
        buf = ctypes.create_string_buffer(size)
        if not ver.GetFileVersionInfoW(path, 0, size, buf):
            return ""
        ptr, n = ctypes.c_void_p(), ctypes.c_uint()
        if not ver.VerQueryValueW(buf, "\\", ctypes.byref(ptr), ctypes.byref(n)) or n.value < 52:
            return ""
        ms, ls = ctypes.cast(ptr.value + 8, ctypes.POINTER(ctypes.c_uint32 * 2)).contents
        return "%d.%d.%d.%d" % (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)
    except (OSError, AttributeError):
        return ""


def compiler_identity(path):
    """What the blobs were compiled with: the DLL's sha256, size and file version (Astra 2026-10-20: the AOT blobs'
    equivalence to a runtime compile is pinned to this compiler, not assumed)."""
    data = open(path, "rb").read()
    return {"dll": os.path.basename(path), "sha256": hashlib.sha256(data).hexdigest(), "size": len(data),
            "version": file_version(path)}


def check_pin(ident, pin_path):
    """The build fails when the compiler is not the pinned one (update the pin deliberately, with a replay check)."""
    pin = json.load(open(pin_path, encoding="utf-8"))
    if pin.get("sha256", "").lower() != ident["sha256"] or int(pin.get("size", -1)) != ident["size"]:
        raise SystemExit("host_hlsl_aot: %s is not the pinned compiler (%s): sha256 %s size %d, pinned sha256 %s size %s "
                         "- the AOT blobs must come from the pinned d3dcompiler; update %s deliberately (and re-run "
                         "the replay 9/9) if the build machine's compiler changed"
                         % (ident["dll"], pin_path, ident["sha256"], ident["size"], pin.get("sha256"), pin.get("size"),
                            pin_path))


def blob_bytes(p):
    """ID3DBlob -> bytes (vtable: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize), then Release."""
    vt = ctypes.cast(ctypes.cast(p, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])
    data = ctypes.string_at(get_ptr(p), get_size(p))
    release(p)
    return data


def compile_one(fn, v):
    src = ctypes.create_string_buffer(v["src"], len(v["src"]))
    macros = None
    if v["defines"]:
        arr = (Macro * (len(v["defines"]) + 1))()
        for i, (mn, mv) in enumerate(v["defines"]):
            arr[i].Name, arr[i].Definition = mn.encode(), (mv or "").encode()
        macros = ctypes.cast(arr, ctypes.c_void_p)
    code, err = ctypes.c_void_p(), ctypes.c_void_p()
    hr = fn(ctypes.cast(src, ctypes.c_void_p), len(v["src"]), v["name"].encode(), macros, None, v["entry"].encode(),
            v["target"].encode(), v["flags1"], 0, ctypes.byref(code), ctypes.byref(err))
    msg = blob_bytes(err).decode("utf-8", "replace") if err.value else ""
    if hr < 0 or not code.value:
        raise SystemExit("host_hlsl_aot: %s failed (hr=0x%08X): %s" % (v["label"], hr & 0xFFFFFFFF, msg.strip()))
    return blob_bytes(code)


def write_unit(path, rows, oracle, dll, ident=None):
    ident = ident or {"dll": os.path.basename(dll), "sha256": "?", "size": 0, "version": ""}
    lines = ["// GENERATED by tools/regen/host_hlsl_aot.py at build time - do not edit (step 4 item 7a, gfx/host_hlsl.hpp).",
             "// %d ahead-of-time host shader blob(s)%s, compiled with %s." % (
                 len(rows), " (oracle-only sources: the SR_ORACLE=1 build)" if oracle else "", os.path.basename(dll)),
             "// compiler: %s sha256 %s size %d version %s" % (ident["dll"], ident["sha256"], ident["size"],
                                                               ident["version"] or "?"),
             '#include "gfx/host_hlsl.hpp"', "", "namespace {"]
    for i, (v, data) in enumerate(rows):
        lines.append("// %s" % v["label"])
        lines.append("const unsigned char b%d[%d] = {" % (i, len(data)))
        for j in range(0, len(data), 24):
            lines.append("  " + ",".join(str(x) for x in data[j:j + 24]) + ",")
        lines.append("};")
    lines.append("const sr::gfx::HostHlslBlob kTable[] = {")
    for i, (v, data) in enumerate(rows):
        lines.append('  {0x%016XULL, b%d, %d, "%s"},' % (v["key"], i, len(data), v["label"]))
    if not rows:
        lines.append("  {0, nullptr, 0, nullptr},")
    lines.append("};")
    lines.append("const bool kRegistered = sr::gfx::registerHostHlslTable(kTable, %d);" % len(rows))
    lines.append("}  // namespace")
    text = "\n".join(lines) + "\n"
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:   # always written: it is the build step's output stamp
        f.write(text)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="", help="the source tree (or give every source with --file)")
    ap.add_argument("--file", action="append", default=[], metavar="REL=ABS",
                    help="read the source REL (as UNITS names it) from ABS; cmake/host_hlsl.cmake passes each file so "
                         "the build edge's COMMAND names those files, not the folder holding the build tree")
    ap.add_argument("--out")
    ap.add_argument("--out-oracle")
    ap.add_argument("--dll", default="")
    ap.add_argument("--pin", default="", help="a JSON pin of the compiler (sha256 + size): a different DLL fails the "
                    "build (cmake/host_hlsl.cmake passes tools/regen/d3dcompiler_47.pin.json)")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--only", choices=("ship", "oracle"), help="write just this half (--out for ship, --out-oracle for "
                    "oracle): cmake/host_hlsl.cmake runs one edge per half, each depending on its own sources")
    a = ap.parse_args(argv)
    for kv in a.file:
        rel, _, ab = kv.partition("=")
        FILES[rel.replace("\\", "/")] = ab
    if not a.root and not a.file:
        ap.error("--root or --file is required")
    vs = variants(a.root, a.only)
    if a.list:
        for v in vs:
            print("%016X %s%s (%d bytes of source)" % (v["key"], v["label"], " [oracle]" if v["oracle"] else "",
                                                       len(v["src"])))
        return 0
    if (a.only != "oracle" and not a.out) or (a.only != "ship" and not a.out_oracle):
        ap.error("--out (ship) and / or --out-oracle (oracle) are required (or --list)")
    fn, dll = compiler(a.dll)
    ident = compiler_identity(dll)
    if a.pin:
        check_pin(ident, a.pin)
    ship, orc = [], []
    for v in vs:
        (orc if v["oracle"] else ship).append((v, compile_one(fn, v)))
    if a.only != "oracle":
        write_unit(a.out, ship, False, dll, ident)
    if a.only != "ship":
        write_unit(a.out_oracle, orc, True, dll, ident)
    print("host_hlsl_aot: %d shipping + %d oracle-only blob(s) (%s)" % (len(ship), len(orc), dll))
    return 0


if __name__ == "__main__":
    sys.exit(main())
