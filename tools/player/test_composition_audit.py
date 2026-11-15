#!/usr/bin/env python3
"""test_composition_audit.py - fixtures and mutants for tools/player/composition_audit.py (v3).

An audit that says PASS for the wrong reasons is worse than none, so this test proves, on a synthetic build (a
build.ninja with its bindings, `ninja -t deps` output, real minimal PE32+ images with import, delay-import and CodeView
records, a real minimal MSF PDB with its DBI module list plus the matching llvm-pdbutil dump, a build-dir copy of the
DLL and a package staged with the packager's manifest):
  * the complete clean fixture PASSES, with every unit, library member and DLL attributed;
  * every detector FIRES on its mutant (exit 1 naming the item) - including each bypass of Astra's v2 review: a library
    or a build-output library on a DLL's (or a static library's) link line, a custom command's output linked directly,
    an unclassified PRE_LINK step, excluded content read by a review (diag) unit, a generated header's producing edge
    consuming undeclared / declared-excluded / #include-reached material, a declared input missing from the edge, a
    build-dir file with no producing edge and no `produced`, an extensionless or numeric-type .rc payload, a
    substituted DLL (build output or vendor pin), a declared dynamic DLL not staged, a packaged file changed after
    staging, a sibling `header` class applied to a .inc, a sibling taken from a glob rule, a content `off` class with a
    reader compiled with SR_ORACLE=1;
  * missing or broken evidence is INCOMPLETE, never PASS: no / stale / miscounted deps, no package, a substitute
    package (no packager manifest), no PDB, a dump without the PDB, a failed llvm-pdbutil, a dump without a Modules
    section or truncated against the PDB's DBI stream, an unparsable .rc, an unreadable build-dir DLL;
  * -DSR_ORACLE: the last effective definition counts (-U between), conflicting definitions are an error; a content
    rule's `off` class applies only when every reader was compiled with SR_ORACLE=0;
  * order-only prerequisites are not linked; UTF-16 deps (a PowerShell redirect) parse like UTF-8.

    python tools/player/test_composition_audit.py      # exit 0 = baseline PASS and every mutant detected
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("composition_audit", os.path.join(HERE, "composition_audit.py"))
ca = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ca)

GUID_BYTES = bytes(range(16))
GUID_TEXT = "{03020100-0504-0706-0809-0A0B0C0D0E0F}"


def make_pe(imports=(), delay=(), guid=GUID_BYTES, age=1, extra=b"", subsystem=0):
    """A minimal PE32+ image: one section holding the import / delay-import descriptors, a CodeView RSDS record and
    the strings (+ `extra` bytes, for marker tests)."""
    sec_rva, sec_raw = 0x1000, 0x200
    body = bytearray()
    strings = {}
    names = list(imports) + list(delay)
    imp_off = 0
    body += b"\0" * (20 * (len(imports) + 1))
    dly_off = len(body)
    body += b"\0" * (32 * (len(delay) + 1))
    dbg_off = len(body)
    body += b"\0" * 28
    rsds_off = len(body)
    body += b"RSDS" + guid + struct.pack("<I", age) + b"x.pdb\0"
    for n in names:
        strings[n] = len(body)
        body += n.encode("ascii") + b"\0"
    body += extra
    for i, n in enumerate(imports):
        struct.pack_into("<I", body, imp_off + 20 * i + 12, sec_rva + strings[n])
    for i, n in enumerate(delay):
        struct.pack_into("<I", body, dly_off + 32 * i + 4, sec_rva + strings[n])
    # IMAGE_DEBUG_DIRECTORY: Type 2 (CodeView) at +12, SizeOfData +16, AddressOfRawData +20, PointerToRawData +24
    struct.pack_into("<IIII", body, dbg_off + 12, 2, 24 + 6, sec_rva + rsds_off, sec_raw + rsds_off)
    pe = 0x80
    img = bytearray(sec_raw) + body
    img[0:2] = b"MZ"
    struct.pack_into("<I", img, 0x3C, pe)
    img[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", img, pe + 4, 0x8664, 1, 0, 0, 0, 240, 0x22)
    opt = pe + 24
    struct.pack_into("<H", img, opt, 0x20B)
    struct.pack_into("<H", img, opt + 68, subsystem)
    struct.pack_into("<I", img, opt + 108, 16)
    dd = opt + 112
    struct.pack_into("<II", img, dd + 8 * 1, sec_rva + imp_off, 20 * (len(imports) + 1))
    struct.pack_into("<II", img, dd + 8 * 6, sec_rva + dbg_off, 28)
    struct.pack_into("<II", img, dd + 8 * 13, sec_rva + dly_off, 32 * (len(delay) + 1))
    sec = opt + 240
    img[sec:sec + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", img, sec + 8, len(body), sec_rva, len(body), sec_raw)
    return bytes(img)


def make_pdb(guid, age, modules, bs=512):
    """A minimal MSF 7.00 PDB: stream 1 (PDB info: version, signature, age, GUID) and stream 3 (DBI: header +
    module-info records)."""
    pdbi = struct.pack("<III", 20000404, 0x12345678, age) + guid
    modinfo = b""
    for name, obj in modules:
        rec = bytearray(64) + name.encode() + b"\0" + obj.encode() + b"\0"
        while len(rec) % 4:
            rec += b"\0"
        modinfo += bytes(rec)
    dbi = bytearray(64)
    struct.pack_into("<iII", dbi, 0, -1, 19990903, age)
    struct.pack_into("<i", dbi, 24, len(modinfo))
    streams = [b"", pdbi, b"", bytes(dbi) + modinfo]
    nxt, data, lists = 4, [], []
    for s in streams:
        idx = []
        for i in range(0, len(s), bs):
            idx.append(nxt)
            data.append((nxt, s[i:i + bs]))
            nxt += 1
        lists.append(idx)
    directory = struct.pack("<I", len(streams)) + b"".join(struct.pack("<I", len(s)) for s in streams) + \
        b"".join(struct.pack("<%dI" % len(b), *b) for b in lists)
    dir_idx = []
    for i in range(0, len(directory), bs):
        dir_idx.append(nxt)
        data.append((nxt, directory[i:i + bs]))
        nxt += 1
    img = bytearray(bs * nxt)
    sb = ca.MSF_MAGIC + struct.pack("<6I", bs, 1, nxt, len(directory), 0, 3)
    img[0:len(sb)] = sb
    struct.pack_into("<%dI" % len(dir_idx), img, 3 * bs, *dir_idx)
    for b, d in data:
        img[b * bs:b * bs + len(d)] = d
    return bytes(img)


NINJA = """# CMAKE generated file: DO NOT EDIT!
include CMakeFiles/rules.ninja
cmake_ninja_workdir = C$:/w/src/build/

build CMakeFiles/App.dir/app/main.cpp.obj: CXX_COMPILER__App_Release C$:/w/src/app/main.cpp || cmake_object_order_depends_target_App
  DEFINES = -DNDEBUG MAINDEF
  FLAGS = -O3 MAINFLAGS
build CMakeFiles/App.dir/gfx/native_draw.cpp.obj: CXX_COMPILER__App_Release C$:/w/src/gfx/native_draw.cpp
build CMakeFiles/App.dir/gfx/diag_$ watch.cpp.obj: CXX_COMPILER__App_Release C$:/w/src/gfx/diag$ watch.cpp
build CMakeFiles/App.dir/app/icon.rc.res: RC_COMPILER__App_Release C$:/w/src/app/icon.rc
  FLAGS = RCFLAGS
build generated/embed.cpp: CUSTOM_COMMAND C$:/w/src/tools/gen.py GENINPUT
  COMMAND = cmd.exe /C "python C:/w/src/tools/gen.py GENCMD -o C:/w/src/build/generated/embed.cpp"
build CMakeFiles/App.dir/generated/embed.cpp.obj: CXX_COMPILER__App_Release generated/embed.cpp
build generated/sh/fxaa_cs.h | ${cmake_ninja_workdir}generated/sh/fxaa_cs.h: CUSTOM_COMMAND C$:/w/src/shaders/fxaa.cs.hlsl $
    C$:/w/src/shaders/fxaa.hlsli C$:/w/src/shaders/edram.xesli FXAAIN
  COMMAND = cmd.exe /C "fxc.exe /I C:/w/src/shaders /Fh C:/w/src/build/generated/sh/fxaa_cs.h /T cs_5_1 C:/w/src/shaders/fxaa.cs.hlsl FXAACMD"
build generated/sh/resolve_cs.h: CUSTOM_COMMAND C$:/w/src/shaders/resolve.cs.xesl C$:/w/src/shaders/edram.xesli
  COMMAND = cmd.exe /C "fxc.exe /Fh C:/w/src/build/generated/sh/resolve_cs.h C:/w/src/shaders/resolve.cs.xesl"
build CMakeFiles/lib.dir/gfx/shader_xlat.cpp.obj: CXX_COMPILER__lib_Release C$:/w/src/gfx/shader_xlat.cpp
  DEFINES = XLATDEF
build CMakeFiles/lib.dir/LIBEXTRA.obj: CXX_COMPILER__lib_Release C$:/w/src/LIBEXTRA
  DEFINES = EXTRADEF
build lib.lib: CXX_STATIC_LIBRARY_LINKER__lib_Release CMakeFiles/lib.dir/gfx/shader_xlat.cpp.obj $
    CMakeFiles/lib.dir/LIBEXTRA.obj
  LINK_FLAGS = STATICFLAG
build CMakeFiles/emu.dir/gfx/emu.cpp.obj: CXX_COMPILER__emu_Release C$:/w/src/gfx/emu.cpp
build sub/emu.lib: CXX_STATIC_LIBRARY_LINKER__emu_Release CMakeFiles/emu.dir/gfx/emu.cpp.obj
build CMakeFiles/dec.dir/C_/deps/FFmpeg/libavcodec/xma.c.obj: C_COMPILER__dec_Release C$:/deps/FFmpeg/libavcodec/xma.c
build dec.dll dec.lib: C_SHARED_LIBRARY_LINKER__dec_Release CMakeFiles/dec.dir/C_/deps/FFmpeg/libavcodec/xma.c.obj
  LANGUAGE_COMPILE_FLAGS = -O3 -DNDEBUG -Xclang --dependent-lib=msvcrt
  LINK_FLAGS = -shared -fuse-ld=lld-link -Xlinker /DEF:CMakeFiles\\dec.dir\\.\\exports.def DLLFLAG
  LINK_LIBRARIES = -lbcrypt -lkernel32 DLLLIB
  LINK_PATH = -LC:/w/src/build/sub
  PRE_LINK = DLLPRELINK
build gen_obj.obj: CUSTOM_COMMAND C$:/w/src/tools/gen.py GENOBJIN
  COMMAND = python C:/w/src/tools/gen.py -o C:/w/src/build/gen_obj.obj
build CMakeFiles/tool.dir/tools/only_order.cpp.obj: CXX_COMPILER__tool_Release C$:/w/src/tools/only_order.cpp
build tool.exe: CXX_EXECUTABLE_LINKER__tool_Release CMakeFiles/tool.dir/tools/only_order.cpp.obj
build shaders: phony generated/sh/fxaa_cs.h generated/sh/resolve_cs.h
build empty.exe: phony
build App.exe: CXX_EXECUTABLE_LINKER__App_Release CMakeFiles/App.dir/app/main.cpp.obj $
    CMakeFiles/App.dir/gfx/native_draw.cpp.obj CMakeFiles/App.dir/gfx/diag_$ watch.cpp.obj $
    CMakeFiles/App.dir/app/icon.rc.res CMakeFiles/App.dir/generated/embed.cpp.obj | lib.lib dec.lib $
    vcpkg/lib/SDL3.lib EXEIMPLICIT || shaders tool.exe dec.dll
  FLAGS = -O3 -DNDEBUG -Xclang --dependent-lib=msvcrt
  LINK_FLAGS = -Xlinker /subsystem:console -Xlinker /DEBUG -Xlinker /DELAYLOAD:dec.dll -fuse-ld=lld-link -Wl,/errorlimit:0 LINKFLAG
  LINK_LIBRARIES = lib.lib  dec.lib  vcpkg/lib/SDL3.lib  -lkernel32 LINKLIB
  PRE_LINK = cd .
build build.ninja: RERUN_CMAKE C$:/w/src/CMakeLists.txt | C$:/w/src/xr_src/xr.h
"""
PRELINK = "cmake.exe -E __create_def C:\\w\\src\\build\\CMakeFiles\\dec.dir\\.\\exports.def " \
          "C:\\w\\src\\build\\CMakeFiles\\dec.dir\\.\\exports.def.objs"
MODS = [("C:\\w\\src\\build\\CMakeFiles\\App.dir\\app\\main.cpp.obj",) * 2,
        ("C:\\w\\src\\build\\CMakeFiles\\App.dir\\gfx\\native_draw.cpp.obj",) * 2,
        ("shader_xlat.cpp.obj", "C:\\w\\src\\build\\lib.lib"),
        ("KERNEL32.dll", "C:\\Kits\\um\\x64\\kernel32.lib"),
        ("exe_main.obj", "C:\\VC\\lib\\x64\\msvcrt.lib"),
        ("dec.dll", "C:\\w\\src\\build\\dec.lib"),
        ("internal .obj file created from .res files",
         "C:\\w\\src\\build\\internal .obj file created from .res files"),
        ("* Linker *", "")]
SDL3 = make_pe(imports=("KERNEL32.dll",), extra=b"sdl3")
DXC = make_pe(imports=("KERNEL32.dll",), extra=b"dxc")


def sha(b):
    return hashlib.sha256(b).hexdigest()


def manifest():
    return {
        "classes": {"native": "ship", "platform": "ship", "translation": "ship", "audio-decode": "ship",
                    "toolchain": "ship", "feature": "ship", "build-tool": "tool", "prerequisite": "tool",
                    "emulation": "exclude", "oracle": "exclude", "mixed": "exclude", "diag": "review"},
        "sources": [
            {"path": "app/main.cpp", "class": "platform"},
            {"path": "gfx/native_draw.cpp", "class": "native"},
            {"path": "gfx/diag watch.cpp", "class": "diag"},
            {"path": "app/icon.rc", "class": "platform"},
            {"path": "<build>/generated/embed.cpp", "class": "translation", "inputs": ["data/*.bin"]},
            {"glob": "gfx/shader_*.cpp", "class": "translation"},
            {"path": "gfx/emu.cpp", "class": "emulation", "why": "interprets packets",
             "markers": ["[PM4_ENGINE]", "SR_EMU_GATE\u0000"]},
            {"path": "gfx/split.cpp", "class": "mixed", "off": "native", "why": "native + its CHECK"},
            {"path": "gfx/withhdr.cpp", "class": "mixed", "header": "native", "why": "native + its CHECK"},
            {"glob": "*/FFmpeg/libav*", "class": "audio-decode"},
        ],
        "content": [
            {"glob": "<build>/generated/sh/fxaa_*", "class": "feature", "inputs": ["shaders/fxaa*"],
             "prerequisites": ["shaders/*"]},
            {"glob": "<build>/generated/sh/resolve_*", "class": "emulation", "why": "EDRAM resolve"},
            {"glob": "<build>/xr/*", "class": "translation", "produced": "configure: a patched copy of xr_src",
             "inputs": ["xr_src/*"]},
            {"glob": "xr_src/*", "class": "translation"},
            {"glob": "shaders/fxaa*", "class": "feature"},
            {"path": "shaders/edram.xesli", "class": "emulation"},
            {"glob": "shaders/resolve*", "class": "emulation"},
            {"glob": "*/LLVM/lib/clang/*", "class": "toolchain"},
            {"path": "tools/gen.py", "class": "build-tool"},
            {"path": "data/xlat_table.bin", "class": "translation"},
            {"path": "data/edram_model.bin", "class": "emulation"},
            {"path": "app/icon.ico", "class": "platform"},
            {"path": "gfx/emu.hpp", "class": "emulation"},
            {"glob": "emu_shaders/*", "class": "emulation"},
            {"glob": "emu dir/*", "class": "emulation"},
            {"glob": "emu_res/*", "class": "emulation"},
            {"path": "app/blob.bin", "class": "platform"},
            {"path": "kernel/meter.hpp", "class": "oracle", "off": "platform", "why": "SR_ORACLE=0: inline no-ops"},
        ],
        "link_inputs": [{"glob": "<build>/vcpkg/*", "class": "platform", "produced": "vcpkg install (configure)"},
                        {"name": "kernel32.lib", "class": "platform"}, {"name": "msvcrt.lib", "class": "toolchain"},
                        {"name": "bcrypt.lib", "class": "platform"},
                        {"path": "<build>/CMakeFiles/dec.dir/exports.def", "class": "audio-decode",
                         "produced": "the DLL's PRE_LINK step"}],
        "link_steps": [{"glob": "cmake.exe -E __create_def <build>/CMakeFiles/dec.dir/./exports.def*",
                        "class": "build-tool"}],
        "runtime_dlls": {"rules": [{"name": "KERNEL32.dll", "class": "platform", "system": True},
                                   {"name": "SDL3.dll", "class": "platform", "sha256": [sha(SDL3)], "vendor": "test"},
                                   {"name": "dxc.dll", "class": "translation", "sha256": [sha(DXC)], "vendor": "test"},
                                   {"name": "dec.dll", "class": "audio-decode"}],
                         "dynamic": [{"dll": "dxc.dll", "by": "gfx/shader_xlat.cpp", "how": "LoadLibraryW"}]},
        "package": [{"path": "App.exe", "class": "platform"}, {"path": "dec.dll", "class": "audio-decode"},
                    {"path": "SDL3.dll", "class": "platform"}, {"path": "dxc.dll", "class": "translation"},
                    {"path": "rogue.dll", "class": "platform"}, {"path": "manifest.json", "class": "platform"},
                    {"glob": "port_features/*", "class": "feature"}],
    }


FAILS = []


def check(name, cond, detail=""):
    print("%s %s%s" % ("ok  " if cond else "FAIL", name, (" - " + detail[:700]) if detail and not cond else ""))
    if not cond:
        FAILS.append(name)


def run(tmp, libextra="gfx/shader_more.cpp", embed="fxaa_cs.h", header="native_draw.hpp", linkflag="", linklib="",
        maindef="", mainflags="", xlatdef="", extradef="", staticflag="", dllflag="", dlllib="", prelink=PRELINK,
        exeimplicit="", genobjin="", geninput="data/xlat_table.bin", gencmd=None, fxaain="", fxaacmd="", fxaa_hlsli="",
        rc_text='1 ICON "icon.ico"\n', rc_files=None, rcdep=(), rcflags="", src_files=None, chain=0, maindep=(), xlatdep=(), miscount=False, drawvalid="VALID",
        extramod=(), base_mods=None, dump_mods=None, dump_header=True, pdb=True, modules=True, guid=GUID_TEXT, guid_bytes=GUID_BYTES,
        exe_imports=("KERNEL32.dll", "SDL3.dll"), exe_extra=b"", pkg_extra=(), pkg_drop=(), pkg_exe_differs=False,
        dll_extra=b"", pkg_dll_extra=None, build_dll=True, sdl3=SDL3, packager=True, pkg_tamper=False,
        built_exe_sha=None, junction=False, deps=True, deps_utf16=False, package=True, target="App.exe", man=None,
        pdbutil=None):
    """Write the fixture with the mutation points filled in and run the audit. Returns (exit, result, output)."""
    shutil.rmtree(tmp, ignore_errors=True)
    src = os.path.join(tmp, "src")
    for d in ("app", "shaders", "build"):
        os.makedirs(os.path.join(src if d != "build" else tmp, d), exist_ok=True)
    nin = NINJA
    for k, v in (("LIBEXTRA", libextra), ("LINKFLAG", linkflag), ("LINKLIB", linklib), ("MAINDEF", maindef),
                 ("MAINFLAGS", mainflags), ("XLATDEF", xlatdef), ("EXTRADEF", extradef), ("STATICFLAG", staticflag),
                 ("DLLFLAG", dllflag), ("DLLLIB", dlllib), ("DLLPRELINK", prelink), ("EXEIMPLICIT", exeimplicit), ("GENOBJIN", genobjin),
                 ("GENINPUT", "C$:/w/src/" + geninput), ("GENCMD", gencmd if gencmd is not None else
                                                          "C:/w/src/" + geninput),
                 ("FXAAIN", fxaain), ("FXAACMD", fxaacmd), ("RCFLAGS", rcflags)):
        nin = nin.replace(k, v)
    for k in range(chain):  # gen/c0.h <- gen/c1.h <- ... <- gen/c<chain>.h <- data/edram_model.bin
        src_in = "gen/c%d.h" % (k + 1) if k + 1 < chain else "C$:/w/src/data/edram_model.bin"
        nin += "build gen/c%d.h: CUSTOM_COMMAND %s\n  COMMAND = python C:/w/src/tools/gen.py %s\n" % (
            k, src_in, src_in.replace("C$:", "C:"))
    with open(os.path.join(tmp, "build.ninja"), "w", encoding="utf-8") as f:
        f.write(nin)
    d = {
        "CMakeFiles/App.dir/app/main.cpp.obj": ["C:/w/src/app/main.cpp", "C:/w/src/build/generated/sh/fxaa_cs.h",
                                                "C:/Program Files/LLVM/lib/clang/22/include/float.h"] + list(maindep) +
        (["C:/w/src/build/gen/c0.h"] if chain else []),
        "CMakeFiles/App.dir/gfx/native_draw.cpp.obj": ["C:/w/src/gfx/native_draw.cpp",
                                                       "C:/w/src/build/generated/sh/" + embed,
                                                       "C:/w/src/gfx/native_draw.hpp"],
        "CMakeFiles/App.dir/gfx/diag_ watch.cpp.obj": ["C:/w/src/gfx/diag watch.cpp", "C:/w/src/gfx/" + header],
        "CMakeFiles/App.dir/app/icon.rc.res": ["C:/w/src/app/icon.rc"] + list(rcdep),
        "CMakeFiles/App.dir/generated/embed.cpp.obj": ["C:/w/src/build/generated/embed.cpp"],
        "CMakeFiles/lib.dir/gfx/shader_xlat.cpp.obj": ["C:/w/src/gfx/shader_xlat.cpp", "C:/w/src/build/xr/xr.h"] +
        list(xlatdep),
        "CMakeFiles/lib.dir/%s.obj" % libextra: ["C:/w/src/" + libextra],
        "CMakeFiles/emu.dir/gfx/emu.cpp.obj": ["C:/w/src/gfx/emu.cpp"],
        "CMakeFiles/dec.dir/C_/deps/FFmpeg/libavcodec/xma.c.obj": ["C:/deps/FFmpeg/libavcodec/xma.c"],
    }
    text = ""
    for obj, paths in d.items():
        n = len(paths) + (1 if miscount and obj.endswith("main.cpp.obj") else 0)
        valid = drawvalid if obj.endswith("native_draw.cpp.obj") else "VALID"
        text += "%s: #deps %d, deps mtime 1 (%s)\n" % (obj, n, valid) + "".join("    %s\n" % p for p in paths) + "\n"
    with open(os.path.join(tmp, "deps.txt"), "wb") as f:
        f.write(text.encode("utf-16") if deps_utf16 else text.encode("utf-8"))
    with open(os.path.join(src, "app", "icon.rc"), "w") as f:
        f.write(rc_text)
    for name, text in (rc_files or {}).items():
        with open(os.path.join(src, "app", name), "w") as f:
            f.write(text)
    for name, text in (src_files or {}).items():
        os.makedirs(os.path.dirname(os.path.join(src, name)), exist_ok=True)
        with open(os.path.join(src, name), "w") as f:
            f.write(text)
    with open(os.path.join(src, "shaders", "fxaa.cs.hlsl"), "w") as f:
        f.write('#include "fxaa.hlsli"\n')
    with open(os.path.join(src, "shaders", "fxaa.hlsli"), "wb") as f:
        f.write(fxaa_hlsli if isinstance(fxaa_hlsli, bytes) else fxaa_hlsli.encode("utf-8"))
    with open(os.path.join(src, "shaders", "edram.xesli"), "w") as f:
        f.write("// the EDRAM model\n")
    mods = list(MODS if base_mods is None else base_mods) + list(extramod)
    dm = mods if dump_mods is None else dump_mods
    dump = "Summary\n  Age: 1\n  GUID: %s\n\n%s" % (guid, "Modules\n" if dump_header else "") + "".join(
        "  Mod %04d | `%s`:\n  Obj: `%s`:\n" % (i, n, o) for i, (n, o) in enumerate(dm))
    with open(os.path.join(tmp, "modules.txt"), "w", encoding="utf-8") as f:
        f.write(dump)
    with open(os.path.join(tmp, "App.pdb"), "wb") as f:
        f.write(make_pdb(guid_bytes, 1, mods))
    with open(os.path.join(tmp, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(man or manifest(), f)
    exe = make_pe(imports=exe_imports, delay=("dec.dll",), extra=exe_extra)
    with open(os.path.join(tmp, "App.exe"), "wb") as f:
        f.write(exe)
    dec = make_pe(imports=("KERNEL32.dll",), extra=b"dec" + dll_extra)
    if build_dll:
        with open(os.path.join(tmp, "build", "dec.dll"), "wb") as f:
            f.write(dec)
    pdir = os.path.join(tmp, "pkg")
    os.makedirs(os.path.join(pdir, "port_features"))
    staged = {"App.exe": make_pe(imports=("KERNEL32.dll",)) if pkg_exe_differs else
              make_pe(imports=exe_imports, delay=("dec.dll",), extra=exe_extra, subsystem=2),
              "dec.dll": dec if pkg_dll_extra is None else make_pe(imports=("KERNEL32.dll",), extra=pkg_dll_extra),
              "SDL3.dll": sdl3, "dxc.dll": DXC, "port_features/pack.json": b"{}"}
    for p in pkg_extra:
        staged[p] = make_pe()
    for p in pkg_drop:
        staged.pop(p)
    for p, b in staged.items():
        with open(os.path.join(pdir, p), "wb") as f:
            f.write(b)
    if packager:
        pm = {"packager": {"script": "tools/player/make_player_build.ps1"},
              "source": {"build_exe_sha256": built_exe_sha or sha(exe)},
              "files": {p: sha(b) for p, b in staged.items()}}
        with open(os.path.join(pdir, "manifest.json"), "w") as f:
            json.dump(pm, f)
    if pkg_tamper:
        with open(os.path.join(pdir, "port_features", "pack.json"), "wb") as f:
            f.write(b'{"changed": 1}')
    if junction:
        os.makedirs(os.path.join(tmp, "disc"))
        with open(os.path.join(tmp, "disc", "default.xex"), "wb") as f:
            f.write(make_pe())
        import _winapi
        _winapi.CreateJunction(os.path.join(tmp, "disc"), os.path.join(pdir, "Disc"))
    out = os.path.join(tmp, "audit.json")
    argv = ["--ninja", os.path.join(tmp, "build.ninja"), "--manifest", os.path.join(tmp, "manifest.json"),
            "--target", target, "--exe", os.path.join(tmp, "App.exe"), "--source-dir", src,
            "--build-dir", os.path.join(tmp, "build"), "--out", out]
    if deps:
        argv += ["--deps", os.path.join(tmp, "deps.txt")]
    if package:
        argv += ["--package", package if isinstance(package, str) else pdir]
    if pdb:
        argv += ["--pdb", os.path.join(tmp, "App.pdb")]
    if modules:
        argv += ["--modules", os.path.join(tmp, "modules.txt")]
    if pdbutil is not None:
        argv += ["--pdbutil", os.path.join(tmp, "manifest.json")]  # exists; run_pdbutil is replaced below
    saved = ca.run_pdbutil
    if pdbutil is not None:
        ca.run_pdbutil = lambda tool, p: (pdbutil, dump)
    buf, err = io.StringIO(), io.StringIO()
    try:
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
            rc = ca.main(argv)
    finally:
        ca.run_pdbutil = saved
    res = json.load(open(out, encoding="utf-8")) if rc != 2 else None
    return rc, res, buf.getvalue() + err.getvalue()


def failing(res):
    return [ca.what(f) for f in res["failing"]]


def with_rule(section, path, **kw):
    m = manifest()
    rules = m[section]["rules"] if isinstance(m[section], dict) else m[section]
    for r in rules:
        if r.get("path") == path or r.get("glob") == path or r.get("name") == path:
            for k, v in kw.items():
                if v is None:
                    r.pop(k, None)
                else:
                    r[k] = v
    return m


SDK_WINRES = "C:/Kits/Include/10.0/um/winres.h"


def rc_approved(*paths):
    """The manifest with .rc-included files approved and the SDK's headers classified."""
    m = manifest()
    m["content"] += [{"path": p, "class": "platform"} for p in paths] + [{"glob": "*/Kits/Include/*",
                                                                           "class": "platform"}]
    return m


def with_content(m, *rules, **top):
    m["content"] += list(rules)
    m.update(top)
    return m


def unit_checks():
    """The include reader and the COMMAND-reference helpers on their own."""
    got = ca.source_includes('#if 0\n# include "a.h"\n#else\n#include <b.h>\n#endif\n/* #include "c.h" */\n'
                             '// #include "d.h"\n# /* x */ include "e.h"\n%:include "f.h"\n??=include "g.h"\n'
                             '#inc\\\nlude "h.h"\n#include_next <i.h>\n#import "j.tlb"\n#include H\n'
                             '#pragma include_alias("k.h", "l.h")\nchar *s = "/*";\n#include "m.h"\n')
    want = {("quote", "a.h"), ("angle", "b.h"), ("quote", "e.h"), ("quote", "f.h"),
            ("quote", "g.h"), ("quote", "h.h"), ("angle", "i.h"), ("quote", "j.tlb"), ("other", "H"),
            ("alias", '#pragma include_alias("k.h", "l.h")'), ("quote", "m.h")}
    check("source_includes: every block, comments read both ways, digraph / trigraph, splices, include_next, "
          "import, a macro form, include_alias", want <= set(got), str(sorted(set(got))))
    check("source_includes: a line-start include inside a block comment still counts",
          ("quote", "c.h") in ca.source_includes('/*\n#include "c.h"\n*/\n'))
    cmd = ca.norm('cmd.exe /C "cd /D C:\\w\\src\\build && fxc.exe /I C:/w/src/shaders /I "C:/w/src/q d" '
                  '..\\data\\a.bin -DSRC=C:/w/src/gen b.txt /FI../inc/./f.h gen/../shaders2 && type C:/w/src/build"')
    refs = ca.command_refs(cmd)
    for path, want in [("c:/w/src/data/a.bin", True), ("c:/w/src/other/b.txt", True), ("c:/w/src/gen/x/y.h", True),
                       ("c:/w/src/inc/f.h", True), ("c:/w/src/x/shaders2/k.h", True), ("c:/w/src/build/out.h", True),
                       ("c:/w/src/shaders/edram.xesli", False), ("c:/w/src/q d/z.h", False),
                       ("c:/w/src/data/xa.bin", False)]:
        check("command_names %s -> %s" % (path, want), ca.command_names(refs, path) is want, str(refs[:3]))
    for c in ("type %SRC%\\x.bin", "type $SRC/x.bin", "copy C:/w/src/*.bin out", "tool @args.rsp"):
        check("an opaque COMMAND names every input: %s" % c, ca.command_names(ca.command_refs(c), "c:/w/z/q.h"))
    check("`cd` as an argument is not a working folder",
          ca.command_names(ca.command_refs("python gen.py cd edram.xesli"), "c:/w/src/shaders/edram.xesli"))


def main():
    # Run outputs stay off C: (AGENTS.md): the worktree's gitignored archive/ when it exists.
    arch = os.path.join(os.path.dirname(os.path.dirname(HERE)), "archive")
    base = tempfile.mkdtemp(prefix="composition_audit_test_", dir=arch if os.path.isdir(arch) else None)
    tmp = os.path.join(base, "t")
    try:
        unit_checks()
        rc, res, out = run(tmp)
        check("complete clean fixture passes", rc == 0 and res["verdict"] == "PASS", out)
        rc_r, res_r, out_r = run(tmp, linkflag="-Xlinker /Brepro -Xlinker /pdbaltpath:%_PDB% -Xlinker /pdbsourcepath:.")
        check("the reproducible-build link flags (step4/repro: /Brepro, /pdbaltpath:, /pdbsourcepath:) select no input",
              rc_r == 0 and res_r["verdict"] == "PASS", out_r)
        # step4/repro: lld-link /pdbsourcepath:. writes the module paths relative to the build directory (".\...")
        build_prefix = "C:\\w\\src\\build\\"
        rel_mods = [(n.replace(build_prefix, ".\\"), o.replace(build_prefix, ".\\")) for n, o in MODS]
        rc_p, res_p, out_p = run(tmp, base_mods=rel_mods)
        check("PDB module paths relative to the build directory (/pdbsourcepath:.) bind to the build's objects",
              rc_p == 0 and res_p["verdict"] == "PASS" and any(o.startswith(".\\CMakeFiles") for _, o in rel_mods),
              out_p)
        rc, res, out = run(tmp)
        srcs = {u["source"]: u["component"] for u in res["units"]}
        check("own, static-library, generated, resource and DLL units are linked",
              set(srcs) == {"app/main.cpp", "gfx/native_draw.cpp", "gfx/diag watch.cpp", "app/icon.rc",
                            "<build>/generated/embed.cpp", "gfx/shader_xlat.cpp", "gfx/shader_more.cpp",
                            "C:/deps/FFmpeg/libavcodec/xma.c"}, str(sorted(srcs)))
        check("a DLL's units are attributed to the DLL", srcs.get("C:/deps/FFmpeg/libavcodec/xma.c") == "dec.dll")
        check("an order-only prerequisite is not linked", "tools/only_order.cpp" not in srcs)
        check("an unlinked build-output library is not linked", "gfx/emu.cpp" not in srcs)
        check("a review unit is listed, not failed", [r["source"] for r in res["review"]] == ["gfx/diag watch.cpp"])
        check("the exe's imports and delay imports are read",
              res["exe"]["imports"] == ["KERNEL32.dll", "SDL3.dll"] and res["exe"]["delay_imports"] == ["dec.dll"])
        check("the PDB is read natively and bound to the exe", res["pdb"]["guid"] == GUID_TEXT and
              res["exe"]["codeview"]["guid"] == GUID_TEXT and res["evidence"]["pdb"]["dbi_modules"] == len(MODS),
              str(res["pdb"]) + str(res["evidence"]))
        check("content is classified, the sibling rule included",
              {c["content"]: c["class"] for c in res["content"]}.get("gfx/native_draw.hpp") == "native")
        check("every linker edge is walked (exe, static libraries, DLL)",
              {(l["node"], l["kind"]) for l in res["linkers"]} == {("App.exe", "exe"), ("lib.lib", "lib"),
                                                                   ("dec.dll", "dll")}, str(res["linkers"]))
        check("the DLL edge's link bindings are classified (its /DEF, its libraries)",
              {"<build>/CMakeFiles/dec.dir/exports.def", "bcrypt.lib"} <=
              {x["input"] for x in res["link_inputs"] if x["via"] == "dec.dll"}, str(res["link_inputs"]))
        check("a generated header's incorporated inputs are followed (the #include closure included)",
              {x["input"] for x in res["produced_from"] if x["item"].endswith("fxaa_cs.h")} ==
              {"shaders/fxaa.cs.hlsl", "shaders/fxaa.hlsli"}, str(res["produced_from"]))
        check("every packaged DLL is bound (build output or vendor pin)",
              {b["dll"]: b["verdict"] for b in res["dll_binding"]} == {"dec.dll": "bound", "SDL3.dll": "bound",
                                                                      "dxc.dll": "bound"}, str(res["dll_binding"]))
        check("the packager's Subsystem edit is accepted (the rest of the exe identical)",
              res["evidence"]["package"]["exe"].startswith("identical but the PE Subsystem"), str(res["evidence"]))
        check("a declared dynamic DLL is required and found", any(
            r["dll"] == "dxc.dll" and r["how"].startswith("dynamic") for r in res["runtime_dlls"]))

        mutants = [
            # v2's detectors
            ("excluded unit inside a static library", dict(libextra="gfx/emu.cpp"), ["gfx/emu.cpp"]),
            ("unclassified unit", dict(libextra="gfx/mystery.cpp"), ["gfx/mystery.cpp"]),
            ("unknown library on the link line", dict(linklib="-lemulator"), ["emulator.lib"]),
            ("unknown linker flag", dict(linkflag="-Xlinker /WHOLEARCHIVE:emu.lib"), ["/WHOLEARCHIVE:emu.lib"]),
            ("a response file on the link line", dict(linklib="@link.rsp"), ["@link.rsp"]),
            ("excluded content read by a shipping unit", dict(embed="resolve_cs.h"),
             ["<build>/generated/sh/resolve_cs.h"]),
            ("unclassified content", dict(header="mystery.hpp"), ["gfx/mystery.hpp"]),
            ("a mixed unit's header without its class", dict(header="split.hpp"), ["gfx/split.hpp"]),
            ("a generated unit's excluded production input", dict(geninput="data/edram_model.bin"),
             ["data/edram_model.bin"]),
            ("an unclassified .rc payload", dict(rc_text='1 ICON "logo.bmp"\n'), ["app/logo.bmp"]),
            ("a PDB module from an unclassified library", dict(extramod=[("emu_core.obj", "C:\\w\\src\\build\\emu.lib")]),
             ["emu_core.obj"]),
            ("step4/repro: a RELATIVE PDB module path (/pdbsourcepath:.) from an unclassified library",
             dict(extramod=[("emu_core.obj", ".\\emu.lib")]), ["emu_core.obj"]),
            ("a closure library member that is no closure unit",
             dict(extramod=[("hidden_emu.cpp.obj", "C:\\w\\src\\build\\lib.lib")]), ["hidden_emu.cpp.obj"]),
            ("a PDB that is not the exe's", dict(guid="{FFFFFFFF-0504-0706-0809-0A0B0C0D0E0F}",
                                                guid_bytes=b"\xff" * 4 + GUID_BYTES[4:]), None),
            ("an unclassified runtime DLL", dict(exe_imports=("KERNEL32.dll", "SDL3.dll", "xenia_core.dll")),
             ["xenia_core.dll"]),
            ("a packaged executable approved only by a glob", dict(pkg_extra=("port_features/emu.dll",)),
             ["port_features/emu.dll"]),
            ("a packaged exe that is not the audited exe", dict(pkg_exe_differs=True), ["App.exe"]),
            ("a marker string in a packaged DLL", dict(dll_extra=b"[PM4_ENGINE] x"), ["[PM4_ENGINE]"]),
            # Astra v2, finding 1: link inputs of every linker edge, build outputs joining the closure
            ("1 a library on the DLL's link line", dict(dlllib="-lemulator"), ["emulator.lib"]),
            ("1 an unknown linker flag on the DLL's link line", dict(dllflag="-Xlinker /WHOLEARCHIVE:x.lib"),
             ["/WHOLEARCHIVE:x.lib"]),
            ("1 a build-output library only on the DLL's link line joins the closure", dict(dlllib="sub/emu.lib"),
             ["gfx/emu.cpp"]),
            ("1 a bare -l name resolving to a build output through -L", dict(dlllib="-lemu"), ["gfx/emu.cpp"]),
            ("1 a library merged by a static library's flags", dict(staticflag="sub/emu.lib"), ["gfx/emu.cpp"]),
            ("1 a custom command's output linked directly", dict(exeimplicit="gen_obj.obj"), ["<build>/gen_obj.obj"]),
            ("1 a classified generated link input whose producing edge consumes excluded material",
             dict(exeimplicit="gen_obj.obj", genobjin="C$:/w/src/data/edram_model.bin", man=dict(manifest(), link_inputs=(
                 manifest()["link_inputs"] + [{"path": "<build>/gen_obj.obj", "class": "platform"}]))),
             ["data/edram_model.bin"]),
            ("1 an unclassified PRE_LINK step", dict(prelink="python C:\\w\\src\\tools\\patch_obj.py"), None),
            # finding 2: content
            ("2 excluded content read by a review (diag) unit", dict(header="emu.hpp"), ["gfx/emu.hpp"]),
            ("2 a generated header's edge consuming undeclared excluded material",
             dict(fxaain="C$:/w/src/data/edram_model.bin"), ["data/edram_model.bin"]),
            ("2 a generated header's declared input that is excluded",
             dict(fxaain="C$:/w/src/data/edram_model.bin", man=with_rule(
                 "content", "<build>/generated/sh/fxaa_*", inputs=["shaders/fxaa*", "data/*"])),
             ["data/edram_model.bin"]),
            ("2 excluded material reached by #include from a declared input",
             dict(fxaa_hlsli='#include "edram.xesli"\n'), ["shaders/edram.xesli"]),
            ("2 a prerequisite the producing COMMAND names", dict(fxaacmd="C:/w/src/shaders/edram.xesli"),
             ["shaders/edram.xesli"]),
            ("2 a declared input missing from the producing edge", dict(man=with_rule(
                "content", "<build>/generated/sh/fxaa_*", inputs=["shaders/fxaa*", "shaders/missing*"])),
             ["<build>/generated/sh/fxaa_cs.h"]),
            ("2 an undeclared non-tool input of a generated unit",
             dict(geninput="data/xlat_table.bin C$:/w/src/gfx/emu.hpp"), ["gfx/emu.hpp"]),
            ("2 a build-dir file with no producing edge and no `produced`",
             dict(man=with_rule("content", "<build>/xr/*", produced=None)), ["<build>/xr/xr.h"]),
            ("2 a configure-produced file declaring an input the configure lacks",
             dict(man=with_rule("content", "<build>/xr/*", inputs=["xr_other/*"])), ["<build>/xr/xr.h"]),
            ("2 an extensionless, unquoted .rc payload", dict(rc_text='1 ICON "icon.ico"\n2 RCDATA payload\n'),
             ["app/payload"]),
            ("2 a numeric-type .rc payload", dict(rc_text='1 ICON "icon.ico"\n3 256 MOVEABLE "blob"\n'),
             ["app/blob"]),
            ("2 an .rc #include", dict(rc_text='#include "res.h"\n1 ICON "icon.ico"\n',
                                       rc_files={"res.h": "#define IDI_APP 1\n"}), ["app/res.h"]),
            # Astra split review, P1: the include / resource closure, relative command references
            ("P1 an angle #include of excluded material from a declared input",
             dict(fxaa_hlsli="#include <edram.xesli>\n"), ["shaders/edram.xesli"]),
            ("P1 an #include_next of excluded material", dict(fxaa_hlsli="#include_next <edram.xesli>\n"),
             ["shaders/edram.xesli"]),
            ("P1 an excluded #include is followed whatever its conditional block",
             dict(fxaa_hlsli='#define EDRAM 0\n#if EDRAM\n#include "edram.xesli"\n#endif\n'), ["shaders/edram.xesli"]),
            ("P1 a resource statement in an approved .rc-included file",
             dict(rc_text='#include "res.h"\n1 ICON "icon.ico"\n', rc_files={"res.h": '2 RCDATA "payload"\n'},
                  man=rc_approved("app/res.h")), ["app/payload"]),
            ("P1 a resource statement in an angle-included .rc file",
             dict(rc_text='#include <res.h>\n1 ICON "icon.ico"\n', rc_files={"res.h": '2 RCDATA "payload"\n'},
                  man=rc_approved("app/res.h")), ["app/payload"]),
            ("P1 a resource statement two .rc includes deep",
             dict(rc_text='#include "res.h"\n1 ICON "icon.ico"\n',
                  rc_files={"res.h": '#include "more.rc2"\n', "more.rc2": '3 RCDATA "blob2"\n'},
                  man=rc_approved("app/res.h", "app/more.rc2")), ["app/blob2"]),
            ("P1 a prerequisite the COMMAND names by a relative path", dict(fxaacmd="../shaders/edram.xesli"),
             ["shaders/edram.xesli"]),
            ("P1 a prerequisite the COMMAND names by its bare name", dict(fxaacmd="edram.xesli"),
             ["shaders/edram.xesli"]),
            ("P1 a prerequisite the COMMAND names through a folder holding it", dict(fxaacmd="--all C:/w/src/shaders"),
             ["shaders/edram.xesli"]),
            ("P1 a prerequisite inside a flag's absolute path", dict(fxaacmd="-DEXTRA=C:/w/src/shaders/edram.xesli"),
             ["shaders/edram.xesli"]),
            ("P1 a wildcard in the COMMAND (what it names is unknown)", dict(fxaacmd="C:/w/src/shaders/*.xesli"),
             ["shaders/edram.xesli"]),
            ("P1 a response file in the COMMAND", dict(fxaacmd="@shaders.rsp"), ["shaders/edram.xesli"]),
            # Astra split review 2, P1: include search, the evaluator gone, COMMAND references
            ("P1b #include_next past the includer's folder to an excluded same-named header",
             dict(fxaacmd="/I C:/w/src/emu_shaders", fxaa_hlsli="#include_next <fxaa.hlsli>\n",
                  src_files={"emu_shaders/fxaa.hlsli": "// emu\n"}), ["emu_shaders/fxaa.hlsli"]),
            ("P1b a quoted /I folder holding an excluded header beside a local same-named one",
             dict(fxaacmd='/I "C:/w/src/emu dir"', fxaa_hlsli='#include "common.xesli"\n',
                  src_files={"shaders/common.xesli": "", "emu dir/common.xesli": ""},
                  man=with_rule("content", "<build>/generated/sh/fxaa_*", inputs=["shaders/fxaa*", "shaders/common*"])),
             ["emu dir/common.xesli"]),
            ("P1b an excluded include in a block whose condition is false (every block counts)",
             dict(fxaa_hlsli='#if -1 > 0U\n#include "edram.xesli"\n#endif\n'), ["shaders/edram.xesli"]),
            ("P1b an include a comment hides is still followed",
             dict(fxaa_hlsli='/*\n#include "edram.xesli"\n*/\n'), ["shaders/edram.xesli"]),
            ("P1b an include a comment splits", dict(fxaa_hlsli='# /* x */ include "edram.xesli"\n'),
             ["shaders/edram.xesli"]),
            ("P1b a digraph include", dict(fxaa_hlsli='%:include "edram.xesli"\n'), ["shaders/edram.xesli"]),
            ("P1b an include reached only through an ancestor includer's folder",
             dict(fxaa_hlsli='#include "sub/inner.h"\n', src_files={"shaders/sub/inner.h": '#include "edram.xesli"\n'},
                  man=with_rule("content", "<build>/generated/sh/fxaa_*", inputs=["shaders/fxaa*", "shaders/sub/*"])),
             ["shaders/edram.xesli"]),
            ("P1b a forced include (/FI) of excluded material off the edge",
             dict(fxaacmd="/FI C:/w/src/emu_shaders/forced.h", src_files={"emu_shaders/forced.h": ""}),
             ["emu_shaders/forced.h"]),
            ("P1b a macro-gated .rc include's excluded payload",
             dict(rc_text='#ifdef NEVER\n#include "emu.rc2"\n#endif\n1 ICON "icon.ico"\n',
                  rc_files={"emu.rc2": '5 RCDATA "edram_blob"\n'}, man=rc_approved("app/emu.rc2")),
             ["app/edram_blob"]),
            ("P1b an RCINCLUDE's payload", dict(rc_text='RCINCLUDE emu.rc2\n1 ICON "icon.ico"\n',
                                                 rc_files={"emu.rc2": '5 RCDATA "edram_blob"\n'},
                                                 man=rc_approved("app/emu.rc2")), ["app/edram_blob"]),
            ("P1b an .rc payload found through its /I folder",
             dict(rcflags="-I C:/w/src/emu_res", rc_text='1 ICON "icon.ico"\n2 RCDATA "blob.bin"\n',
                  src_files={"emu_res/blob.bin": "", "app/blob.bin": ""}), ["emu_res/blob.bin"]),
            ("P1b a relative reference with a ./ component", dict(fxaacmd="&& type ../shaders/./edram.xesli >> x"),
             ["shaders/edram.xesli"]),
            ("P1b an attached relative flag operand", dict(fxaacmd="/FI../shaders/edram.xesli"),
             ["shaders/edram.xesli"]),
            ("P1b a bare folder reference", dict(fxaacmd="&& type shaders"), ["shaders/edram.xesli"]),
            ("P1b a folder named after `cd` to it", dict(fxaacmd="&& cd /D C:/w/src/shaders && type C:/w/src/shaders"),
             ["shaders/edram.xesli"]),
            ("P1b a tree file the COMMAND names that is not on its edge",
             dict(fxaacmd="&& type C:/w/src/data/edram_model.bin", src_files={"data/edram_model.bin": ""}),
             ["data/edram_model.bin"]),
            ("P1b an exemption never hides a tree file",
             dict(fxaa_hlsli='#include "edram.xesli"\n', man=with_content(manifest(), unresolved_includes=[
                 {"name": "edram.xesli", "from": "shaders/fxaa.hlsli", "why": "x"}])), ["shaders/edram.xesli"]),
            # Astra split review 3, P1: quoted paths, hidden directives, rc macros, deep chains
            ("P1c a quoted COMMAND path holding a space",
             dict(fxaacmd='&& type "C:/w/src/emu dir/model.bin" >> x', src_files={"emu dir/model.bin": ""}),
             ["emu dir/model.bin"]),
            ("P1c an include behind a UTF-8 byte-order mark",
             dict(fxaa_hlsli=b'\xef\xbb\xbf#include "edram.xesli"\n'), ["shaders/edram.xesli"]),
            ("P1c an include in a UTF-16 file", dict(fxaa_hlsli=b"\xff\xfe" + '#include "edram.xesli"\n'.encode(
                "utf-16-le")), ["shaders/edram.xesli"]),
            ("P1c form-feed / vertical-tab directive whitespace", dict(fxaa_hlsli='\v#\finclude "edram.xesli"\n'),
             ["shaders/edram.xesli"]),
            ("P1c a ??/ trigraph line continuation", dict(fxaa_hlsli='#inc??/\nlude "edram.xesli"\n'),
             ["shaders/edram.xesli"]),
            ("P1c a splice with whitespace after the backslash", dict(fxaa_hlsli='#inc\\ \nlude "edram.xesli"\n'),
             ["shaders/edram.xesli"]),
            ("P1c CR-only line ends", dict(fxaa_hlsli='// x\r#include "edram.xesli"\r'), ["shaders/edram.xesli"]),
            ("P1c an .rc payload named by a macro (approving the macro's name)",
             dict(rc_text='#define BLOB "edram_blob"\n1 ICON "icon.ico"\n2 RCDATA BLOB\n', man=rc_approved("app/BLOB")),
             ["app/edram_blob"]),
            ("P1c an .rc payload macro defined in an included header",
             dict(rc_text='#include "res.h"\n1 ICON "icon.ico"\n2 RCDATA BLOB\n',
                  rc_files={"res.h": '#define BLOB "edram_blob"\n'}, man=rc_approved("app/res.h", "app/BLOB")),
             ["app/edram_blob"]),
            ("P1c an .rc payload macro from the command line",
             dict(rcflags="-DBLOB=edram_blob", rc_text='1 ICON "icon.ico"\n2 RCDATA BLOB\n', man=rc_approved("app/BLOB")),
             ["app/edram_blob"]),
            ("P1c excluded material at the end of a 20-deep generated chain",
             dict(chain=20, man=with_content(manifest(), {"glob": "<build>/gen/c*", "class": "translation",
                                                          "inputs": ["<build>/gen/c*", "data/*"]})),
             ["data/edram_model.bin"]),
            # Astra split review 4, P1: quoted flag values, function-like rc macros; RCINCLUDE by macro
            ("P1d a quoted flag value holding a space",
             dict(gencmd='"--input=C:/w/src/emu dir/model.bin"', src_files={"emu dir/model.bin": ""}),
             ["emu dir/model.bin"]),
            ("P1d an RCINCLUDE named by a macro", dict(rc_text='#define EMU emu.rc2\nRCINCLUDE EMU\n1 ICON "icon.ico"\n',
                                                    rc_files={"emu.rc2": '5 RCDATA "edram_blob"\n'},
                                                    man=rc_approved("app/emu.rc2")), ["app/edram_blob"]),
            # finding 3: binary / package
            ("3 a substituted build-output DLL", dict(pkg_dll_extra=b"other"), ["dec.dll"]),
            ("3 a substituted vendor DLL", dict(sdl3=make_pe(imports=("KERNEL32.dll",), extra=b"evil")), ["SDL3.dll"]),
            ("3 a packaged DLL bound to nothing", dict(pkg_extra=("rogue.dll",)), ["rogue.dll"]),
            ("3 a declared dynamic DLL not staged", dict(pkg_drop=("dxc.dll",)), ["dxc.dll"]),
            ("3 an env-gate marker (NUL-exact) in the exe", dict(exe_extra=b"SR_EMU_GATE\0"), None),
            ("3 a package file changed after staging", dict(pkg_tamper=True), ["manifest.json"]),
            ("3 a packager manifest for another build's exe", dict(built_exe_sha="0" * 64), ["manifest.json"]),
            # finding 5: siblings, content `off`
            ("5 a sibling's header class applied to a .inc", dict(header="withhdr.inc"), ["gfx/withhdr.inc"]),
            ("5 a sibling taken from a glob sources rule", dict(header="shader_more.hpp"), ["gfx/shader_more.hpp"]),
            ("5 a content `off` class with a reader compiled with SR_ORACLE=1",
             dict(xlatdep=["C:/w/src/kernel/meter.hpp"], xlatdef="-DSR_ORACLE=0",
                  maindep=["C:/w/src/kernel/meter.hpp"]), ["kernel/meter.hpp"]),
        ]
        for name, kw, expect in mutants:
            rc, res, out = run(tmp, **kw)
            ok = rc == 1 and res["verdict"] == "FAIL" and (expect is None or all(e in failing(res) for e in expect))
            check("MUTANT " + name, ok, out)
        rc, res, out = run(tmp, exe_imports=("KERNEL32.dll", "SDL3.dll", "dec2.dll"), man=dict(
            manifest(), runtime_dlls={"rules": manifest()["runtime_dlls"]["rules"] + [
                {"name": "dec2.dll", "class": "audio-decode"}], "dynamic": []}))
        check("MUTANT a non-system DLL that is not staged", rc == 1 and any(
            f["status"] == "unresolved" and f.get("dll") == "dec2.dll" for f in res["failing"]), out)
        rc, res, out = run(tmp, exe_extra=b"SR_EMU_GATE_MORE\0")
        check("an env-gate marker matches only the exact C string", rc == 0, out)
        # The real release rule must catch a restored XMA array selector even
        # though the rest of its translation unit is native when SR_ORACLE=0.
        actual_path = os.path.join(HERE, "..", "..", "docs", "release_composition.json")
        with open(actual_path, encoding="utf-8") as stream:
            actual = json.load(stream)
        array_rule = next(r for r in actual["sources"] if r.get("path") == "kernel/audio_native_census.cpp")
        check("release XMA rule names the exact forbidden selector",
              "SR_XMA_ARRAY_NATIVE\0" in array_rule.get("markers", []), repr(array_rule))
        m = manifest()
        m["sources"].append(array_rule)
        rc, res, out = run(tmp, man=m, exe_extra=b"SR_XMA_ARRAY_NATIVE\0")
        check("MUTANT real XMA selector restored to release binary",
              rc == 1 and any(f.get("marker") == "SR_XMA_ARRAY_NATIVE\\0" for f in res["failing"]), out)
        if os.name == "nt":
            rc, res, out = run(tmp, junction=True)
            check("MUTANT 3 a link / junction in the package without a link rule", rc == 1 and "Disc" in failing(res),
                  out)
            m = manifest()
            m["package"].append({"path": "Disc", "class": "platform", "link": True})
            rc, res, out = run(tmp, junction=True, man=m)
            check("a junction with a link rule is recorded, never followed", rc == 0 and not any(
                p["file"].startswith("Disc/") for p in res["package_files"]), out)

        for name, kw in [("no deps", dict(deps=False)), ("stale deps", dict(drawvalid="STALE")),
                         ("no package", dict(package=False)), ("no PDB", dict(pdb=False, modules=False)),
                         # finding 3: the substitute package; a build-output DLL that cannot be compared
                         ("a substitute package (no packager manifest)", dict(packager=False)),
                         ("an unreadable build-dir DLL", dict(build_dll=False)),
                         # finding 4: evidence integrity
                         ("a deps record whose #deps count differs from its paths", dict(miscount=True)),
                         ("a module dump without the PDB", dict(pdb=False)),
                         ("llvm-pdbutil exiting non-zero", dict(modules=False, pdbutil=1)),
                         ("a module dump without a Modules section", dict(dump_header=False)),
                         ("a module dump truncated against the PDB's DBI stream", dict(dump_mods=MODS[:-2])),
                         ("an unparsable .rc statement", dict(rc_text='1 ICON\n')),
                         # Astra split review, P1
                         ("an unresolved angle #include in a taken block", dict(fxaa_hlsli="#include <nowhere.h>\n")),
                         ("an unresolved #include in a block a closure file's #define decides",
                          dict(fxaa_hlsli="#define MSL 1\n#if MSL\n#include <metal_stdlib>\n#endif\n")),
                         ("an unresolved #include in a block a compiler predefine decides",
                          dict(fxaa_hlsli="#if __SHADER_TARGET_MAJOR > 5\n#include <sm6.h>\n#endif\n")),
                         ("an unsupported #include form", dict(fxaa_hlsli="#include SHADER_HEADER\n")),
                         ("an unresolved .rc #include", dict(rc_text='#include "missing.h"\n1 ICON "icon.ico"\n')),
                         ("an .rc SDK #include missing from the compile's deps",
                          dict(rc_text='#include <winres.h>\n1 ICON "icon.ico"\n')),
                         ("a macro include (#define H ...; #include H)",
                          dict(fxaa_hlsli='#define H "edram.xesli"\n#if -1 > 0U\n#include H\n#endif\n')),
                         ("an include_alias pragma", dict(fxaa_hlsli='#pragma include_alias("a.h", "edram.xesli")\n')),
                         ("an unresolved include an exemption names for another includer",
                          dict(fxaa_hlsli="#include <metal_stdlib>\n", man=with_content(manifest(), unresolved_includes=[
                              {"name": "metal_stdlib", "from": "shaders/other.xesli", "why": "x"}]))),
                         ("a forced include that resolves nowhere", dict(fxaacmd="/FI C:/w/src/shaders/nowhere.h")),
                         ("a source with NUL bytes and no byte-order mark",
                          dict(fxaa_hlsli='#include "a.h"\n'.encode("utf-16-le"))),
                         ("an .rc payload macro with two definitions",
                          dict(rc_text='#define BLOB "a"\n#undef BLOB\n#define BLOB "b"\n1 ICON "icon.ico"\n'
                                       '2 RCDATA BLOB\n', man=rc_approved("app/BLOB"))),
                         ("a function-like macro in an .rc statement (its literal tokens approved)",
                          dict(rc_text='#define F(x) x\n1 ICON "icon.ico"\n2 RCDATA F("blob")\n',
                               man=rc_approved("app/F(", "app/blob"))),
                         ("a function-like macro invoked as F(1) (its literal approved)",
                          dict(rc_text='#define F(x) "edram_blob"\n1 ICON "icon.ico"\n2 RCDATA F(1)\n',
                               man=rc_approved("app/F(1)"))),
                         ("an unparsable statement in an .rc-included file",
                          dict(rc_text='#include "res.h"\n1 ICON "icon.ico"\n', rc_files={"res.h": "1 ICON\n"},
                               man=rc_approved("app/res.h")))]:
            rc, res, out = run(tmp, **kw)
            check("INCOMPLETE on " + name, rc == 1 and res["verdict"] == "INCOMPLETE" and res["incomplete"], out)
        rc, res, out = run(tmp, modules=False, pdbutil=0)
        check("llvm-pdbutil exiting 0 with a complete dump is evidence", rc == 0, out)
        rc, res, out = run(tmp, package=os.path.join(base, "missing_pkg"))
        check("a missing package directory is an error", rc == 2, out)
        rc, res, out = run(tmp, target="empty.exe")
        check("a target with no unit is an error", rc == 2, out)
        rc, res, out = run(tmp, maindef="-DSR_ORACLE=0 -DSR_ORACLE=1")
        check("conflicting -DSR_ORACLE definitions on one object are an error (4)", rc == 2 and "conflicting" in out,
              out)
        rc, res, out = run(tmp, maindef="-DSR_ORACLE=0", mainflags="-DSR_ORACLE=1")
        check("conflicting -DSR_ORACLE across DEFINES and FLAGS are an error (4)", rc == 2, out)

        rc, res, out = run(tmp, libextra="gfx/split.cpp")
        check("a mixed unit fails in the development configuration", rc == 1 and "gfx/split.cpp" in failing(res), out)
        rc, res, out = run(tmp, libextra="gfx/split.cpp", extradef="-DNDEBUG -DSR_ORACLE=0")
        check("a unit compiled with -DSR_ORACLE=0 takes its `off` class", rc == 0 and res["oracle_off_units"] == 1 and
              {u["source"]: u["class"] for u in res["units"]}["gfx/split.cpp"] == "native", out)
        rc, res, out = run(tmp, libextra="gfx/split.cpp", extradef="-DSR_ORACLE=1 -USR_ORACLE -DSR_ORACLE=0")
        check("the last effective -DSR_ORACLE counts (-U between definitions)", rc == 0 and
              res["oracle_off_units"] == 1, out)
        rc, res, out = run(tmp, libextra="gfx/split.cpp", extradef="-DSR_ORACLE=0 -USR_ORACLE")
        check("-USR_ORACLE after -DSR_ORACLE=0 is not the release configuration", rc == 1 and
              "gfx/split.cpp" in failing(res), out)
        rc, res, out = run(tmp, xlatdep=["C:/w/src/kernel/meter.hpp"], xlatdef="-DSR_ORACLE=0")
        check("a content `off` class applies when every reader has SR_ORACLE=0", rc == 0 and
              {c["content"]: c["class"] for c in res["content"]}.get("kernel/meter.hpp") == "platform", out)
        rc, res, out = run(tmp, header="withhdr.hpp")
        check("a mixed unit's real header takes the rule's `header` class", rc == 0 and
              {c["content"]: c["class"] for c in res["content"]}.get("gfx/withhdr.hpp") == "native", out)
        rc, res, out = run(tmp, rc_text='// x\n#include <winres.h>\nLANGUAGE 9, 1\n1 VERSIONINFO FILEVERSION 1,0,0,0\n'
                           'BEGIN\n BLOCK "StringFileInfo"\n BEGIN\n VALUE "a", "b"\n END\nEND\nSTRINGTABLE\nBEGIN\n'
                           ' 1 "x.dll"\nEND\n1 ICON DISCARDABLE "icon.ico"\n', rcdep=[SDK_WINRES], man=rc_approved())
        check("an .rc with blocks / string tables names no false payload", rc == 0, out)
        check("an .rc SDK #include is resolved by the compile's deps and recorded", res["evidence"].get(
            "rc_system_includes") == [{"rc": "app/icon.rc", "include": "winres.h", "from": "app/icon.rc",
                                       "path": SDK_WINRES}], str(res["evidence"]))
        rc, res, out = run(tmp, fxaa_hlsli="#if SHADING_LANGUAGE_MSL_XE\n  #include <metal_stdlib>\n#endif\n",
                           man=with_content(manifest(), unresolved_includes=[
                               {"name": "metal_stdlib", "from": "shaders/fxaa.hlsli", "why": "MSL only"}]))
        check("an unresolved include the manifest exempts (exact name and includer) passes and is recorded",
              rc == 0 and res["evidence"]["include_exemptions"] == [
                  {"include": "metal_stdlib", "from": "shaders/fxaa.hlsli", "into": "<build>/generated/sh/fxaa_cs.h",
                   "why": "MSL only"}], out + str(res["evidence"]))
        rc, res, out = run(tmp, fxaa_hlsli="// the include <nowhere.h> of older SDKs is gone\n")
        check("prose that reads like an include (the loose reading) is not INCOMPLETE when nothing matches", rc == 0,
              out)
        rc, res, out = run(tmp, fxaa_hlsli="// the include <edram.xesli> is gone\n")
        check("... but a tree file the loose reading names joins the closure (fail-closed)",
              rc == 1 and "shaders/edram.xesli" in failing(res), out)
        rc, res, out = run(tmp, fxaacmd="C:/w/src/shaders/fxaa_cs_extra.txt edram.xesli.bak")
        check("a reference to another file is not the prerequisite (no false name)", rc == 0, out)
        rc16, res16, _ = run(tmp, deps_utf16=True)
        rc8, res8, _ = run(tmp)
        check("UTF-16 deps parse like UTF-8", rc16 == 0 and res16["content"] == res8["content"])
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print("test_composition_audit: %s (%d failed)" % ("PASS" if not FAILS else "FAIL", len(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
