#!/usr/bin/env python3
"""composition_audit.py - the C1 release-composition audit (ledger release.composition, RELEASE_PLAN step 2).

C1: no emulation components in the release executable, its shipping DLLs or the package, including disabled paths and
fallback components; emulation and development oracles stay only in separate development builds. An environment gate
does not make a component absent: the audit reads what the build compiled, linked and staged.

    python tools/player/composition_audit.py --ninja <build>/build.ninja --deps <`ninja -t deps` output>
           --exe <exe> --pdb <pdb> --package <dir staged by tools/player/make_player_build.ps1>
           [--build-dir <the build tree's outputs, readable now; default: --ninja's folder>]
           [--target SupermanReturns.exe] [--manifest docs/release_composition.json]
           [--source-dir <the tree the build was configured from, readable now>]
           [--modules <llvm-pdbutil dump -summary -modules output>] [--pdbutil <llvm-pdbutil>] [--out <audit.json>]

Every item is classified by docs/release_composition.json (first matching rule: `path` = exact, `glob` = fnmatch,
`name` = case-insensitive basename). A class's status is `ship`, `exclude` (fails C1), `review` (an owner decision:
listed, not failed) or `tool` (build-tool / prerequisite: allowed only as a non-incorporated input of a producing
edge). An item no rule classifies FAILS as unknown. A unit compiled with an effective -DSR_ORACLE=0 (the release
configuration, kernel/sr_oracle.hpp; the last -D / -U of DEFINES then FLAGS wins, two different -D values with no -U
between them are an error) takes its rule's `off` class; a content item takes its rule's `off` class only when EVERY
unit that read it was compiled with SR_ORACLE=0.

 1 LINKED UNITS AND LINK INPUTS: from the target's link edge, every edge reached through explicit / implicit inputs
   AND through the link bindings (FLAGS / LANGUAGE_COMPILE_FLAGS / LINK_FLAGS / LINK_LIBRARIES / LINK_PATH) of EVERY
   linker edge in the closure - the exe, each DLL edge, each static library - is visited, to a fixpoint: a library on
   any link line with a build edge (a path, or a bare name the linker would find in the build dir or a -L / /LIBPATH
   dir) joins the closure; one without needs a `link_inputs` rule. An unknown linker flag or token, or a response
   file, FAILS. /DEF, /MANIFESTINPUT and /DEFAULTLIB name inputs. A linker edge's PRE_LINK steps (other than `cd`)
   need a `link_steps` rule. A custom command's output linked directly is a generated link input (below).
 2 CONTENT: every file a linked unit's compile read (ninja deps: headers, .inc, the PCH, generated headers, embedded
   bytecode) and every file of an .rc's include closure (#include and RCINCLUDE, as below) with every payload its
   resource statements name (parsed in each closure file: name, type keyword / number / user type, options, quoted
   or unquoted filename - an extension is not required; each found at every candidate folder: the .rc's, the
   stating file's, the -I folders, the build dir) needs a `content` rule; an include outside the trees must be in the
   .rc compile's own deps (an SDK header). A real header (.h
   .hh .hpp .hxx) with no rule takes the EXACT `path` sources rule of its same-named .cpp (its `header` class for a
   mixed unit, never a glob); .inc / .inl need their own rule. Excluded content read by ANY retained unit (ship or
   review) FAILS: review permits the unit, never embedded emulation / oracle material.
   GENERATED ITEMS (a unit's source, a content item or a link input that a build edge produces, or that lies in the
   build dir): the producing edge's inputs are split by the item's rule - `inputs` (globs) = the material it
   incorporates (classified; excluded material FAILS; followed if itself generated), `prerequisites` (globs) = edge
   inputs it does not incorporate; every other input must be classified build-tool / prerequisite, else unknown. A
   declared input must exist on the edge (or in the include closure). An input the edge's COMMAND names cannot be a
   prerequisite (ninja order-only inputs are prerequisites unless it does): named by its absolute path or a folder
   holding it, by any relative spelling or bare name or folder name (the working folder is not resolved), attached
   to a flag (/FI../x) - a wildcard, a response file or an environment variable names every input; `cd` operands and
   a preprocessor's include search folders are not references. A tree file the COMMAND names that is not on its edge
   must be a declared input or classified build-tool. For a preprocessing producer (fxc, dxc, clang, cl, rc ...) the
   include closure of the incorporated sources and forced includes (/FI, -include) must be declared too. The closure
   over-approximates every compiler's search instead of modelling one: every include of every closure file, in
   every conditional block, in every reading the audit knows (BOM / UTF-16 / UTF-32 decoded, CR line ends, with and
   without trigraphs, splices also with trailing whitespace, comments kept and removed, and a loose scan for
   include-like text anywhere), resolved against every candidate folder (the search and working folders, every
   closure file's folder), every existing candidate joining it, to a fixpoint; a directive include with no
   candidate, a macro include or an include_alias pragma is INCOMPLETE unless `unresolved_includes` exempts that
   exact name from that exact includer (an exemption never hides a tree file); a source with NUL bytes and no BOM
   is INCOMPLETE. A quoted COMMAND operand may hold spaces. The chain of generated inputs is followed to its end
   (each item once). An .rc's statements are read with its object-like macros (closure files, -D) expanded. A
   build-dir file with no producing edge (a configure-time or link-step output) needs a rule declaring `produced`;
   its `inputs` must be inputs of the configure (the RERUN_CMAKE edge).
 3 LINKED MODULES: every module the linker put in the image is a closure object, a member of a closure static library
   matching a closure unit, a closure DLL's import library, a member of a classified external library, the linker's
   own or the compiled resources - anything else FAILS. The module list is read from the PDB itself (its DBI stream)
   and from the llvm-pdbutil dump when there is one; the dump must come from a zero exit, have a Modules section with
   modules 0..N-1 each with its Obj line, and agree with the DBI stream - otherwise INCOMPLETE. The PDB's GUID / age
   must be the exe's CodeView GUID / age.
 4 RUNTIME DLLS: the exe's and every packaged DLL's import and delay-import tables, and the manifest's
   runtime_dlls.dynamic list (DLLs the code loads with LoadLibrary): each DLL needs a runtime_dlls rule and is either a
   system DLL or staged in the package. Every packaged DLL is bound to an audited artifact: a closure DLL (sha256
   equal to the build dir's file the edge produced) or a vendor artifact pinned by sha256 in its rule; anything else
   FAILS. /DELAYLOAD on any linker edge must show in that binary's delay imports.
 5 PACKAGE: every staged file is classified; an executable file (.exe .dll .sys .pyd .com .scr .ocx .cpl .efi) needs
   an exact path rule; a link / junction is never followed and needs a rule with "link": true. The package's exe is the
   audited exe (byte-identical, or differing only in the PE Subsystem field the packager sets). A package not staged
   by tools/player/make_player_build.ps1 (no manifest.json naming it) is INCOMPLETE; a packager manifest whose files,
   hashes or build exe differ from the package FAILS.
 6 MARKERS: the marker strings of every excluded rule (log tags, env-gate names with their NUL) must not occur in the
   exe or a packaged DLL.

Verdict: FAIL when an item fails; otherwise INCOMPLETE when evidence is missing or not intact (no / stale / miscounted
deps records, no PDB, a bad module dump, no package or a substitute package, an unreadable .rc or #include, a build
output DLL whose build-dir copy is unreadable); PASS only with complete, intact evidence and no failing item. Writes
--out (default <ninja dir>/composition_audit.json). Exit 0 PASS, 1 FAIL or INCOMPLETE, 2 error (unreadable input, a
target with no linked unit, conflicting -DSR_ORACLE definitions on one object).
"""
import argparse
import fnmatch
import hashlib
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_MANIFEST = os.path.join(REPO, "docs", "release_composition.json")
PACKAGER = "tools/player/make_player_build.ps1"
COMPILE_RULE_PREFIXES = ("CXX_COMPILER__", "C_COMPILER__", "RC_COMPILER__", "ASM_COMPILER__")
STATIC_RULE_PREFIXES = ("CXX_STATIC_LIBRARY_LINKER__", "C_STATIC_LIBRARY_LINKER__")
SHARED_RULE_PREFIXES = ("CXX_SHARED_LIBRARY_LINKER__", "C_SHARED_LIBRARY_LINKER__")
EXEC_RULE_PREFIXES = ("CXX_EXECUTABLE_LINKER__", "C_EXECUTABLE_LINKER__")
LINK_RULE_PREFIXES = STATIC_RULE_PREFIXES + SHARED_RULE_PREFIXES + EXEC_RULE_PREFIXES
EXEC_EXT = (".exe", ".dll", ".sys", ".pyd", ".com", ".scr", ".ocx", ".cpl", ".efi")
REAL_HEADER_EXT = (".h", ".hh", ".hpp", ".hxx")
SOURCE_EXT = (".c", ".cc", ".cpp", ".cxx")
LINK_INPUT_EXT = (".lib", ".a", ".obj", ".o", ".res", ".def", ".manifest")
LINK_VARS = ("FLAGS", "LANGUAGE_COMPILE_FLAGS", "LINK_FLAGS", "LINK_LIBRARIES", "LINK_PATH")
RETAINED = ("ship", "review")
# Linker flags that select no input (lld-link spellings; matched case-insensitively on the flag's name).
KNOWN_LINK_FLAGS = ("/subsystem:", "/manifest:embed", "/manifest", "/manifestinput:", "/stack:", "/debug", "/delayload:",
                    "/opt:", "/incremental", "/ltcg", "/pdb:", "/implib:", "/machine:", "/nologo", "/version:",
                    "/dynamicbase", "/nxcompat", "/highentropyva", "/largeaddressaware", "/map", "/ignore:",
                    "/guard:", "/release", "/brepro", "/timestamp:", "/lldltocache:", "/opt:lldltojobs=", "/errorlimit:",
                    # step4/repro (SR_REPRODUCIBLE): the path the exe names its PDB by; the base the PDB's relative
                    # paths are made absolute against
                    "/pdbaltpath:", "/pdbsourcepath:")
# Linker flags that name an input file (the input is then classified like any other).
INPUT_LINK_FLAGS = ("/manifestinput:", "/def:", "/defaultlib:")
# Driver flags on a link line that select no input.
DRIVER_FLAG_RES = (r"-O\d?|-Os|-Oz", r"-D\S+", r"-flto(=\S+)?", r"-fuse-ld=\S+", r"-g\S*", r"-m\S+",
                   r"-fms-runtime-lib=\S+", r"-nostartfiles", r"-nostdlib", r"-shared", r"-fsanitize=\S+",
                   r"-fno-omit-frame-pointer")
# A producing COMMAND running one of these follows #include: the incorporated sources' include closure is checked.
PREPROCESSOR_RE = re.compile(r"(?:^|[/\s\"])(fxc|dxc|clang|clang\+\+|clang-cl|cl|gcc|g\+\+|cpp|rc|llvm-rc|glslc|"
                             r"glslangvalidator)(?:\.exe)?(?=[\s\"]|$)", re.I)
# An #include-like directive (#, the %: digraph or the ??= trigraph) and an MSVC include_alias pragma (it remaps
# include names: unsupported).
INCLUDE_DIRECTIVE_RE = re.compile(r"^[^\S\n]*(?:#|%:|\?\?=)[^\S\n]*(include_next|include|import)\b[^\S\n]*(.*)$",
                                  re.M)
INCLUDE_ALIAS_RE = re.compile(r"^[^\S\n]*(?:#|%:|\?\?=)[^\S\n]*pragma[^\S\n]+include_alias\b.*$", re.M | re.I)
# An include-like word followed by a quoted / angled name ANYWHERE in the text (the loose reading).
LOOSE_INCLUDE_RE = re.compile(r'(?:include_next|include|import)[^\S\n]*(?:"([^"\n]+)"|<([^>\n]+)>)')
TRIGRAPHS = {"??=": "#", "??/": "\\", "??'": "^", "??(": "[", "??)": "]", "??!": "|", "??<": "{", "??>": "}",
             "??-": "~"}
SOURCE_BOMS = ((b"\xef\xbb\xbf", "utf-8"), (b"\xff\xfe\x00\x00", "utf-32-le"), (b"\x00\x00\xfe\xff", "utf-32-be"),
               (b"\xff\xfe", "utf-16-le"), (b"\xfe\xff", "utf-16-be"))
# A preprocessing COMMAND's include search folders and forced includes (the operand quoted or not, attached or not).
SEARCH_FLAG_RE = re.compile(r'(?:^|(?<=[\s"]))(?:/external:I|-isystem|-iquote|-idirafter|-imsvc|[/-]I(?!nclude|macros))'
                            r'\s*("[^"]*"|[^\s"]+)', re.I)
FORCED_FLAG_RE = re.compile(r'(?:^|(?<=[\s"]))(?:/FI|(?i:-include|--include|-imacros))(?:=|\s*)("[^"]*"|[^\s"]+)')
CD_RE = re.compile(r'(^|[&|(]|/c\s+")\s*cd(?:\s+/d)?\s+("[^"]*"|[^\s"&|)]+)', re.I)
# A path-like run of a COMMAND, and an absolute path inside one (-SD:/x, /FID:/x, -DSRC=D:/x).
CMD_PATH_RE = re.compile(r"[^\s\"'<>|&;,=()]+")
CMD_ABS_RE = re.compile(r"[a-z]:/[^\s\"'<>|&;,=()]*", re.I)
# .rc resource statements.
RC_BLOCK_TYPES = {"DIALOG", "DIALOGEX", "MENU", "MENUEX", "ACCELERATORS", "VERSIONINFO", "TOOLBAR", "DESIGNINFO",
                  "TEXTINCLUDE"}
RC_OPTIONS = {"PRELOAD", "LOADONCALL", "FIXED", "MOVEABLE", "DISCARDABLE", "PURE", "IMPURE", "SHARED", "NONSHARED"}
MSF_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"


# ------------------------------------------------------------------------------------------ build.ninja

def split_ninja(text):
    """Logical ninja lines: `$` + newline continues (the next line's indent dropped); `#` lines skipped."""
    out, cur = [], ""
    for raw in text.splitlines():
        line = raw.lstrip() if cur else raw
        if line.endswith("$") and not line.endswith("$$"):
            cur += line[:-1]
            continue
        cur += line
        if cur.strip() and not cur.lstrip().startswith("#"):
            out.append(cur)
        cur = ""
    if cur.strip():
        out.append(cur)
    return out


def tokens(s):
    """Split on unescaped spaces; `$ `, `$:`, `$$` unescape. A bare `:` / `|` / `||` / `|@` stays a token."""
    toks, cur, i = [], "", 0
    while i < len(s):
        c = s[i]
        if c == "$" and i + 1 < len(s):
            cur += s[i + 1] if s[i + 1] in " :$" else "$" + s[i + 1]
            i += 2
            continue
        if c == " ":
            if cur:
                toks.append(cur)
            cur = ""
        elif c == ":" and not cur.endswith(":") and (i + 1 == len(s) or s[i + 1] == " "):
            if cur:
                toks.append(cur)
            toks.append(":")
            cur = ""
        else:
            cur += c
        i += 1
    if cur:
        toks.append(cur)
    return toks


def norm(p):
    return p.replace("\\", "/")


def is_abs(p):
    return (len(p) > 1 and p[1] == ":") or p.startswith("/")


def parse_ninja(text):
    """({output: edge}, workdir). edge = {rule, outputs, explicit, implicit, order_only, vars}. workdir = CMake's
    cmake_ninja_workdir (the build directory the file was generated for, wherever the copy now lives)."""
    edges, workdir, cur = {}, "", None
    for line in split_ninja(text):
        if line[:1].isspace():
            if cur is not None and "=" in line:
                k, v = line.strip().split("=", 1)
                cur["vars"][k.strip()] = v.strip()
            continue
        cur = None
        if line.startswith("cmake_ninja_workdir"):
            workdir = "".join(tokens(line.split("=", 1)[1].strip()))
            continue
        if not line.startswith("build "):
            continue
        t = [x.replace("${cmake_ninja_workdir}", workdir).replace("$cmake_ninja_workdir", workdir)
             for x in tokens(line[len("build "):])]
        if ":" not in t:
            continue
        k = t.index(":")
        outs = [o for o in t[:k] if o != "|"]
        rest = t[k + 1:]
        if not rest:
            continue
        edge = {"rule": rest[0], "outputs": outs, "explicit": [], "implicit": [], "order_only": [], "vars": {}}
        part = "explicit"
        for tok in rest[1:]:
            if tok == "|":
                part = "implicit"
            elif tok == "||":
                part = "order_only"
            elif tok == "|@":
                part = None
            elif part:
                edge[part].append(tok)
        for o in outs:
            edges[o] = edge
        cur = edge
    return edges, norm(workdir).rstrip("/")


def parse_deps(path):
    """`ninja -t deps` -> {object: {"valid", "declared" (the record's #deps count, None if unparsable), "deps"}}
    (UTF-16 from a PowerShell redirect accepted)."""
    raw = open(path, "rb").read()
    text = raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else raw.decode("utf-8", "replace")
    deps, cur = {}, None
    for line in text.splitlines():
        if not line.strip():
            continue
        if not line[0].isspace():
            head, _, tail = line.partition(": #deps ")
            m = re.match(r"(\d+), deps mtime \d+ \((\w+)\)\s*$", tail)
            cur = head.strip()
            deps[cur] = {"valid": bool(m) and m.group(2) == "VALID", "declared": int(m.group(1)) if m else None,
                         "deps": []}
        elif cur is not None:
            deps[cur]["deps"].append(line.strip())
    return deps


def sr_oracle_off(edge, node):
    """True when the compile's effective SR_ORACLE is 0: DEFINES, INCLUDES then FLAGS in command order; the last -D /
    -U wins; two different -D values without a -U between them are rejected (ValueError)."""
    toks = []
    for k in ("DEFINES", "INCLUDES", "FLAGS"):
        toks += tokens(edge["vars"].get(k, ""))
    state, i = None, 0
    while i < len(toks):
        t = toks[i]
        i += 1
        if t in ("-D", "/D", "-U", "/U") and i < len(toks):
            t = t + toks[i]
            i += 1
        m = re.fullmatch(r"[-/]DSR_ORACLE(?:=(.*))?", t)
        if m:
            val = m.group(1) if m.group(1) is not None else "1"
            if state is not None and state != val:
                raise ValueError("conflicting -DSR_ORACLE definitions on %s: %s then %s (no -U between them)" % (
                    node, state, val))
            state = val
        elif re.fullmatch(r"[-/]USR_ORACLE", t):
            state = None
    return state == "0"


# ------------------------------------------------------------------------------------------ PE / PDB

class PE:
    """The few PE32+ facts the audit needs: imports, delay imports, the CodeView (RSDS) GUID / age, the Subsystem
    field's offset."""

    def __init__(self, path=None, data=None):
        self.data = data if data is not None else open(path, "rb").read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError("%s: not a PE file" % path)
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("%s: no PE signature" % path)
        nsec, opt_size = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        self.subsystem_off = opt + 68
        magic = struct.unpack_from("<H", d, opt)[0]
        dd = opt + (112 if magic == 0x20B else 96)
        ndd = struct.unpack_from("<I", d, dd - 4)[0]
        self.dirs = [struct.unpack_from("<II", d, dd + 8 * i) for i in range(min(ndd, 16))]
        sec = opt + opt_size
        self.sections = []
        for i in range(nsec):
            vsize, va, rsize, raw = struct.unpack_from("<IIII", d, sec + 40 * i + 8)
            self.sections.append((va, max(vsize, rsize), raw))

    def off(self, rva):
        for va, size, raw in self.sections:
            if va <= rva < va + size:
                return raw + rva - va
        return None

    def cstr(self, rva):
        o = self.off(rva)
        if o is None:
            return None
        return self.data[o:self.data.index(b"\0", o)].decode("ascii", "replace")

    def _dir(self, i):
        return self.dirs[i] if i < len(self.dirs) else (0, 0)

    def imports(self):
        rva, size = self._dir(1)
        out, o = [], self.off(rva) if rva else None
        while o is not None and o + 20 <= len(self.data):
            name_rva = struct.unpack_from("<I", self.data, o + 12)[0]
            if not any(self.data[o:o + 20]):
                break
            out.append(self.cstr(name_rva))
            o += 20
        return out

    def delay_imports(self):
        rva, size = self._dir(13)
        out, o = [], self.off(rva) if rva else None
        while o is not None and o + 32 <= len(self.data):
            if not any(self.data[o:o + 32]):
                break
            out.append(self.cstr(struct.unpack_from("<I", self.data, o + 4)[0]))
            o += 32
        return out

    def codeview(self):
        """{guid, age, pdb} of the RSDS debug record, or None."""
        rva, size = self._dir(6)
        o = self.off(rva) if rva else None
        for i in range((size // 28) if o is not None else 0):
            typ, raw = struct.unpack_from("<I", self.data, o + 28 * i + 12)[0], \
                struct.unpack_from("<I", self.data, o + 28 * i + 24)[0]
            if typ == 2 and self.data[raw:raw + 4] == b"RSDS":
                age = struct.unpack_from("<I", self.data, raw + 20)[0]
                end = self.data.index(b"\0", raw + 24)
                return {"guid": fmt_guid(self.data[raw + 4:raw + 20]), "age": age,
                        "pdb": self.data[raw + 24:end].decode("utf-8", "replace")}
        return None


def fmt_guid(g):
    d1, d2, d3 = struct.unpack_from("<IHH", g)
    return "{%08X-%04X-%04X-%s-%s}" % (d1, d2, d3, g[8:10].hex().upper(), g[10:16].hex().upper())


def same_but_subsystem(a, b):
    """True when two PE images differ at most in the 2-byte optional-header Subsystem field (what the packager sets)."""
    if len(a) != len(b):
        return False
    off = PE(data=a).subsystem_off
    return a[:off] == b[:off] and a[off + 2:] == b[off + 2:]


def read_pdb(path):
    """The PDB itself (MSF 7.00): {guid, age} from the PDB info stream and [(module, obj)] from the DBI stream's
    module-info substream - the linker's own module list, independent of any dump tool."""
    with open(path, "rb") as f:
        sb = f.read(56)
        if sb[:32] != MSF_MAGIC:
            raise ValueError("%s: not an MSF 7.00 PDB" % path)
        bs, _, _, dirbytes, _, mapaddr = struct.unpack_from("<6I", sb, 32)

        def blocks(idx):
            out = []
            for b in idx:
                f.seek(b * bs)
                out.append(f.read(bs))
            return b"".join(out)

        nmap = -(-dirbytes // bs)
        if nmap * 4 > bs:
            raise ValueError("%s: stream directory too large" % path)
        f.seek(mapaddr * bs)
        d = blocks(struct.unpack_from("<%dI" % nmap, f.read(bs)))[:dirbytes]
        n = struct.unpack_from("<I", d)[0]
        sizes = struct.unpack_from("<%dI" % n, d, 4)
        off, lists = 4 + 4 * n, []
        for s in sizes:
            k = 0 if s == 0xFFFFFFFF else -(-s // bs)
            lists.append(struct.unpack_from("<%dI" % k, d, off))
            off += 4 * k

        def stream(i):
            if i >= n or sizes[i] == 0xFFFFFFFF:
                raise ValueError("%s: no stream %d" % (path, i))
            return blocks(lists[i])[:sizes[i]]

        info = stream(1)
        age = struct.unpack_from("<I", info, 8)[0]
        guid = fmt_guid(info[12:28])
        dbi = stream(3)
    modsize = struct.unpack_from("<i", dbi, 24)[0]
    mods, o, end = [], 64, 64 + modsize
    while o + 64 < end:
        e1 = dbi.index(b"\0", o + 64)
        e2 = dbi.index(b"\0", e1 + 1)
        mods.append((dbi[o + 64:e1].decode("utf-8", "replace"), dbi[e1 + 1:e2].decode("utf-8", "replace")))
        o = (e2 + 1 + 3) & ~3
    return {"guid": guid, "age": age, "modules": mods}


def parse_pdb_modules(text):
    """llvm-pdbutil dump -modules -> ([(module name, obj path)], [integrity problems])."""
    mods, name, idx, problems = [], None, [], []
    lines = text.splitlines()
    if not any(l.strip() == "Modules" for l in lines):
        problems.append("no Modules section")
    for line in lines:
        m = re.match(r"\s*Mod (\d+) \| `(.*)`:", line)
        if m:
            if name is not None:
                problems.append("Mod %d has no Obj line" % idx[-1])
            idx.append(int(m.group(1)))
            name = m.group(2)
            continue
        m = re.match(r"\s*Obj: `(.*)`:", line)
        if m and name is not None:
            mods.append((name, m.group(1)))
            name = None
    if name is not None:
        problems.append("Mod %d has no Obj line (truncated)" % idx[-1])
    if idx != list(range(len(idx))):
        problems.append("module numbers are not 0..N-1")
    return mods, problems


def pdbutil_path(arg):
    for c in [arg, shutil.which("llvm-pdbutil"), "C:/Program Files/LLVM/bin/llvm-pdbutil.exe"]:
        if c and os.path.exists(c):
            return c
    return None


def run_pdbutil(tool, pdb):
    r = subprocess.run([tool, "dump", "-summary", "-modules", pdb], capture_output=True, text=True)
    return r.returncode, r.stdout


def parse_pdb_summary(text):
    """GUID / age from llvm-pdbutil dump -summary (None when the text has no summary block)."""
    guid = re.search(r"GUID:\s*(\{[0-9A-Fa-f-]+\})", text)
    age = re.search(r"Age:\s*(\d+)", text)
    if not guid:
        return None
    return {"guid": guid.group(1).upper(), "age": int(age.group(1)) if age else None}


# ------------------------------------------------------------------------------------------ .rc files

# ------------------------------------------------------------------------------------------ preprocessing

def strip_comments(text):
    """C / C++ comments removed outside string and character literals (a literal ends at its line's end)."""
    out, i, n, lit = [], 0, len(text), None
    while i < n:
        c = text[i]
        if lit is None:
            if text.startswith("//", i):
                j = text.find("\n", i)
                i = n if j < 0 else j
                continue
            if text.startswith("/*", i):
                j = text.find("*/", i + 2)
                out.append(" ")
                i = n if j < 0 else j + 2
                continue
            if c in "\"'":
                lit = c
        elif c == "\\" and i + 1 < n and text[i + 1] != "\n":
            out.append(text[i:i + 2])
            i += 2
            continue
        elif c in (lit, "\n"):
            lit = None
        out.append(c)
        i += 1
    return "".join(out)


def decode_source(data):
    """Source bytes -> text: a UTF-8 / UTF-16 / UTF-32 byte-order mark honoured (and dropped); NUL bytes without one
    -> None (undecodable: the caller reports it INCOMPLETE); else UTF-8, invalid bytes replaced."""
    for bom, enc in SOURCE_BOMS:
        if data.startswith(bom):
            return data[len(bom):].decode(enc, errors="replace")
    if b"\x00" in data:
        return None
    return data.decode("utf-8", errors="replace")


def pp_texts(text):
    """The readings of a C-preprocessed text an include can hide in: CR / CRLF lines normalised, BOMs dropped, each
    read without and with trigraph replacement (??= ??/ ...), backslash-newline splices removed (also with
    whitespace between them, and ??/ ones), then each as written and with its comments removed."""
    text = text.replace("\r\n", "\n").replace("\r", "\n").replace("\ufeff", "")
    out = []
    for base in (text, re.sub(r"\?\?[=/'()!<>\-]", lambda m: TRIGRAPHS[m.group(0)], text)):
        spliced = re.sub(r"\\[^\S\n]*\n", "", base)
        out += [spliced, strip_comments(spliced)]
    return out


def source_includes(text):
    """The includes of a C-preprocessed text -> [(form, name)]: form quote / angle (a directive: the name), other
    (#include MACRO: its raw text), alias (#pragma include_alias: its line), or quote* / angle* (the LOOSE reading:
    an include-like word followed by a quoted or angled name anywhere - a directive some lexical form hides; its
    candidates join the closure, but having none is not reported, since prose matches too). Fail-closed: every
    conditional block counts, and every reading of pp_texts is scanned."""
    out, loose = [], []
    for txt in pp_texts(text):
        for m in INCLUDE_DIRECTIVE_RE.finditer(txt):
            rest = m.group(2).strip()
            q = re.match(r'"([^"]+)"|<([^>]+)>', rest) or \
                re.match(r'"([^"]+)"|<([^>]+)>', strip_comments(rest).strip())
            out.append(("quote", q.group(1)) if q and q.group(1) else ("angle", q.group(2)) if q else
                       ("other", rest))
        out += [("alias", m.group(0).strip()) for m in INCLUDE_ALIAS_RE.finditer(txt)]
        loose += [("quote*", m.group(1)) if m.group(1) else ("angle*", m.group(2))
                  for m in LOOSE_INCLUDE_RE.finditer(txt)]
    strict = set(out)
    return list(dict.fromkeys(out + [x for x in loose if (x[0][:-1], x[1]) not in strict]))


def ref_norm(p):
    """A relative reference with its . and .. components resolved; a .. above its start (the working folder, which
    is not resolved) is dropped."""
    parts = []
    for c in p.split("/"):
        if c in ("", "."):
            continue
        if c == "..":
            if parts:
                parts.pop()
            continue
        parts.append(c)
    return "/".join(parts)


def command_refs(cmd):
    """The file references of a normalised COMMAND -> (absolute, relative, opaque, tokens, absolute as written).
    `cd` and its operand are dropped (a working folder is not read), and so are a preprocessing command's include
    search-folder operands (-I / /I / -isystem ...: what it reads through them is its #include closure, checked
    apart). Absolute: every absolute path in a token, also attached to a flag (-SD:/x, /FID:/x, -DSRC=D:/x), . and
    .. resolved. Relative: from every other token the token itself, each tail after a '/' (a name behind an
    unknown prefix or variable) and, for a flag token, each tail after its first 1-16 characters that is a
    path - holds a / or a . (an attached operand: /FI../x), . and .. resolved - the working folder is not resolved, so a reference names any file it is
    a path suffix of and any file under a folder it names. Opaque: a wildcard, a response file (@x) or an
    environment variable (%X%, $X, ${X}, $(X)) - what it names is unknown, so it names every input. Absolute and
    relative are lower case (for matching); tokens (the relative tokens, to resolve against the working folders)
    and absolute as written keep their case."""
    cmd = CD_RE.sub(r"\1 ", cmd)
    if PREPROCESSOR_RE.search(cmd):
        cmd = SEARCH_FLAG_RE.sub(" ", cmd)
    opaque = bool(re.search(r"[*?]|(?:^|[\s\"])@\S|%[^%\s]+%|\$[({]?[a-z_]", cmd, re.I))
    absolute, relative, toks, written = set(), set(), set(), set()
    # a quoted operand may hold spaces: every text between two consecutive quotes (both pairings - cmd.exe's /C "..."
    # wraps the whole line) is also a whole-path candidate
    i = cmd.find('"')
    for seg in re.findall(r'"([^"]*)"', cmd) + (re.findall(r'"([^"]*)"', cmd[i + 1:]) if i >= 0 else []):
        s = seg.strip()
        # the segment's path candidates, spaces kept: the whole; what follows each '=' or ',' (a flag's value); a
        # flag's attached operand (after its first 1-16 characters, when it holds a / or a .)
        cands = {s} | {s[k + 1:].strip() for k in range(len(s)) if s[k] in "=,"}
        if s.startswith(("-", "/")):
            cands |= {x for x in (s[k:] for k in range(1, min(len(s), 17))) if "/" in x or "." in x}
        for c in cands:
            for m in re.finditer(r"[a-z]:/", c, re.I):  # every absolute path, to the segment's end
                a = c[m.start():].rstrip()
                written.add(norm(os.path.normpath(a)).rstrip("/"))
                absolute.add(norm(os.path.normpath(a)).lower().rstrip("/"))
            if c and not re.match(r"[a-z]:/", c, re.I) and not c.startswith(("-", "/")):
                toks.add(c)
                relative.update(r for r in (ref_norm(x).lower() for x in
                                            {c} | {c[k + 1:] for k in range(len(c)) if c[k] == "/"}) if r)
    for tok in CMD_PATH_RE.findall(cmd):
        found = CMD_ABS_RE.findall(tok)
        written.update(norm(os.path.normpath(a)).rstrip("/") for a in found)
        absolute.update(norm(os.path.normpath(a)).lower().rstrip("/") for a in found)
        if found:
            continue
        toks.add(tok)
        tails = {tok} | {tok[k + 1:] for k in range(len(tok)) if tok[k] == "/"}
        if tok.startswith(("-", "/")):
            tails |= {x for x in (tok[k:] for k in range(1, min(len(tok), 17))) if "/" in x or "." in x}
        relative.update(r for r in (ref_norm(x).lower() for x in tails) if r)
    return absolute, relative, opaque, toks, written


def command_names(refs, path):
    """Does a COMMAND (command_refs) name the absolute path: as itself or through a folder holding it, or by a
    relative reference it ends with or that names a folder holding it. Fail-closed - an ambiguous reference names
    it."""
    absolute, relative, opaque = refs[:3]
    a = path.lower()
    if opaque or any(a == r or a.startswith(r + "/") for r in absolute):
        return True
    return any(a.endswith("/" + r) or "/" + r + "/" in a for r in relative)


def rc_resources(text, macros=None):
    """A resource script -> (payload files, RCINCLUDE files, problems). Statements: `name type [options] file`
    (type = keyword, number, identifier or quoted string; the file quoted or not, any extension or none) or
    `name type ... BEGIN|{ ... END|}`; LANGUAGE / CHARACTERISTICS / VERSION / STRINGTABLE / RCINCLUDE at top level.
    Preprocessor lines are skipped (source_includes reads their #includes)."""
    includes, lines = [], []
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("#"):
            continue
        i = line.find("//")
        if i >= 0 and line[:i].count('"') % 2 == 0:
            line = line[:i]
        lines.append(line)
    tok_re = r'"(?:[^"]|"")*"|[{},|]|[^\s{},|"]+'
    toks, problems = re.findall(tok_re, "\n".join(lines)), []
    # rc preprocesses before it reads statements: an identifier a closure file / the command line #defines is
    # replaced by its value (object-like, one definition); several definitions or a function-like macro used in a
    # statement cannot be expanded here - a problem (INCOMPLETE)
    for _ in range(16):
        if not macros or not any(x in macros or (re.match(r"([A-Za-z_]\w*)\(", x) and
                                                  re.match(r"([A-Za-z_]\w*)\(", x).group(1) in macros) for x in toks):
            break
        out = []
        for x in toks:
            defs = macros.get(x)
            call = re.match(r"([A-Za-z_]\w*)\(", x)
            if defs is None and call and call.group(1) in macros:
                problems.append("macro %s invoked in a resource statement: function-like" % call.group(1))
                out.append(x)
            elif defs is None:
                out.append(x)
            elif len(defs) != 1 or defs[0][0]:
                problems.append("macro %s in a resource statement: %s" % (
                    x, "function-like" if any(d[0] for d in defs) else "%d definitions" % len(defs)))
                out.append(x)
            else:
                out += re.findall(tok_re, defs[0][1])
        if out == toks:
            break
        toks = out
    else:
        problems.append("macro expansion in a resource statement did not settle")
    files, i, n = [], 0, len(toks)

    def skip_block(j):
        depth = 0
        while j < n:
            t = toks[j].upper()
            if t in ("BEGIN", "{"):
                depth += 1
            elif t in ("END", "}"):
                depth -= 1
                if depth == 0:
                    return j + 1
            j += 1
        problems.append("an unterminated BEGIN / { block")
        return n

    def to_block(j):
        while j < n and toks[j].upper() not in ("BEGIN", "{"):
            j += 1
        return skip_block(j)

    while i < n:
        up = toks[i].upper()
        if up == "RCINCLUDE":
            if i + 1 >= n:
                problems.append("an RCINCLUDE with no file")
                break
            f = toks[i + 1]
            includes.append(norm(f[1:-1].replace('""', '"') if f.startswith('"') else f))
            i += 2
            continue
        if up == "STRINGTABLE":
            i = to_block(i + 1)
            continue
        if up == "LANGUAGE":
            i += 4
            continue
        if up in ("CHARACTERISTICS", "VERSION"):
            i += 2
            continue
        if up in ("BEGIN", "{", "END", "}", ",", "|"):
            problems.append("a stray %r" % toks[i])
            i = skip_block(i) if up in ("BEGIN", "{") else i + 1
            continue
        if i + 1 >= n:
            problems.append("a statement with no resource type: %r" % toks[i])
            break
        typ = toks[i + 1].upper()
        j = i + 2
        if typ in RC_BLOCK_TYPES:
            i = to_block(j)
            continue
        while j < n and toks[j].upper() in RC_OPTIONS:
            j += 1
        if j >= n:
            problems.append("resource %s %s has no file or data" % (toks[i], toks[i + 1]))
            break
        if toks[j].upper() in ("BEGIN", "{"):
            i = skip_block(j)  # inline data (RCDATA / a user type)
            continue
        f = toks[j]
        if f.startswith('"'):
            f = f[1:-1].replace('""', '"').replace("\\\\", "/")
        files.append(norm(f))
        i = j + 1
    return files, includes, problems


# ------------------------------------------------------------------------------------------ the graph

class Graph:
    def __init__(self, edges, workdir, source_root):
        self.edges, self.workdir, self.root = edges, norm(workdir).rstrip("/"), norm(source_root).rstrip("/")
        self.index = {}
        for k in edges:
            self.index.setdefault(self._canon(k), k)
        self.configure = next((e for e in edges.values() if e["rule"] == "RERUN_CMAKE"), None)

    def _canon(self, p):
        p = norm(p)
        if not is_abs(p):
            p = self.workdir + "/" + p
        return norm(os.path.normpath(p)).lower()

    def key(self, p):
        """The edge key of a path (relative to the build dir or absolute, any case / slashes), or None."""
        return self.index.get(self._canon(p))

    def edge(self, p):
        k = self.key(p)
        return self.edges[k] if k is not None else None

    def rel(self, p):
        """`<build>/x` for build-directory files; root-relative for sources; absolute otherwise."""
        p = norm(p)
        if "/./" in p or "/../" in p:
            p = norm(os.path.normpath(p))
        if self.workdir and p.lower().startswith(self.workdir.lower() + "/"):
            return "<build>/" + p[len(self.workdir) + 1:]
        if self.root and p.lower().startswith(self.root.lower() + "/"):
            return p[len(self.root) + 1:]
        if not is_abs(p):
            return "<build>/" + p
        return p

    def absolute(self, node):
        p = norm(node)
        return p if is_abs(p) else self.workdir + "/" + p

    def node_of(self, rel):
        """A classified path (`<build>/x`, root-relative or absolute) back to a graph path."""
        if rel.startswith("<build>/"):
            return self.workdir + "/" + rel[len("<build>/"):]
        if is_abs(rel) or not self.root:
            return rel
        return self.root + "/" + rel

    def generated(self, p):
        """The producing edge of a file: ('edge', edge) for a custom command / alias, ('build', None) for a
        build-dir file nothing produces, None for a source file or a compile / link output."""
        e = self.edge(p)
        if e is not None and not (e["rule"] == "phony" and not (e["explicit"] or e["implicit"])):
            if e["rule"].startswith(COMPILE_RULE_PREFIXES + LINK_RULE_PREFIXES):
                return None
            return ("edge", e)
        if self.rel(p).startswith("<build>/"):
            return ("build", None)
        return None

    def resolve(self, tok, paths):
        """A link-line library -> its build-graph node, or None: a path (relative to the build dir or absolute), or
        a bare name searched in the build dir (the linker's cwd) and the -L / /LIBPATH dirs."""
        k = self.key(tok)
        if k is not None:
            return k
        if "/" not in norm(tok):
            for d in paths:
                k = self.key(norm(d).rstrip("/") + "/" + tok)
                if k is not None:
                    return k
        return None

    def closure(self, target):
        """Units, DLLs, static libraries, external link inputs and linker edges of `target`, to a fixpoint over every
        linker edge's inputs AND link bindings."""
        units, dlls, libs, externals, linkers, seen = [], {}, {}, [], [], set()
        link_fails, delays, steps = [], [], []

        def visit(node, component, lib=None, via=None):
            k = self.key(node) or node
            if (k, component, lib) in seen:
                return
            seen.add((k, component, lib))
            e = self.edges.get(k)
            if e is None or (e["rule"] == "phony" and not (e["explicit"] or e["implicit"])):
                externals.append({"node": node, "component": component, "via": via, "generated": False})
                return
            rule = e["rule"]
            if rule.startswith(COMPILE_RULE_PREFIXES):
                src = e["explicit"][0] if e["explicit"] else k
                units.append({"object": k, "source": self.rel(src), "component": component, "lib": lib,
                              "oracle_off": sr_oracle_off(e, k), "src_node": src})
                return
            if rule.startswith(SHARED_RULE_PREFIXES):
                dll = next((o for o in e["outputs"] if o.lower().endswith(".dll")), e["outputs"][0])
                name = os.path.basename(dll)
                if name not in dlls:
                    dlls[name] = {"output": dll, "units": 0,
                                  "import_libs": [o for o in e["outputs"] if o.lower().endswith(".lib")]}
                    linkers.append({"node": dll, "edge": e, "component": name, "kind": "dll", "lib": None})
                for i in e["explicit"] + e["implicit"]:
                    visit(i, name, None, dll)
                return
            if rule.startswith(STATIC_RULE_PREFIXES):
                if (k, component) not in libs:
                    libs[(k, component)] = True
                    linkers.append({"node": k, "edge": e, "component": component, "kind": "lib", "lib": k})
                for i in e["explicit"] + e["implicit"]:
                    visit(i, component, k, k)
                return
            if rule == "phony":
                for i in e["explicit"] + e["implicit"]:
                    visit(i, component, lib, via)
                return
            externals.append({"node": k, "component": component, "via": via, "generated": True})

        root_key = self.key(target) or target
        root = self.edges.get(root_key)
        if root is None:
            raise ValueError("no build edge for target %r" % target)
        comp = os.path.basename(target)
        linkers.append({"node": root_key, "edge": root, "component": comp, "kind": "exe", "lib": None})
        for i in root["explicit"] + root["implicit"]:
            visit(i, comp, None, root_key)
        n = 0
        while n < len(linkers):
            L = linkers[n]
            n += 1
            items, fails, delay, paths = link_items(L["edge"])
            link_fails += [dict(f, edge=L["node"]) for f in fails]
            delays += [{"edge": L["node"], "kind": L["kind"], "component": L["component"], "dll": d} for d in delay]
            for tok, how in items:
                k = self.resolve(tok, paths)
                if k is not None:
                    visit(k, L["component"], L["lib"], L["node"])
                else:
                    externals.append({"node": tok, "component": L["component"], "via": L["node"], "generated": False,
                                      "how": how})
            for s in L["edge"]["vars"].get("PRE_LINK", "").split("&&"):
                s = s.strip()
                if s and not re.fullmatch(r"cd(\s+/D)?\s+\S+", s):
                    steps.append({"edge": L["node"], "var": "PRE_LINK", "step": s})
        for u in units:
            if u["component"] in dlls:
                dlls[u["component"]]["units"] += 1
        libs = sorted({k for k, _ in libs})
        return units, dlls, libs, externals, linkers, link_fails, delays, steps


def link_items(edge):
    """A linker edge's bindings -> (inputs [(token, how)], failing items, /DELAYLOAD dlls, library search dirs)."""
    toks = []
    for k in LINK_VARS:
        toks += tokens(edge["vars"].get(k, ""))
    items, fails, delay, paths = [], [], [], []

    def flag(f):
        low = f.lower()
        if low.startswith("/delayload:"):
            delay.append(f.split(":", 1)[1])
        elif low.startswith("/libpath:"):
            paths.append(f.split(":", 1)[1])
        elif low.startswith(INPUT_LINK_FLAGS):
            items.append((f.split(":", 1)[1], low.split(":", 1)[0]))
        elif not low.startswith(KNOWN_LINK_FLAGS):
            fails.append({"input": f, "status": "unknown", "class": None, "why": "unknown linker flag"})

    i = 0
    while i < len(toks):
        t = toks[i]
        i += 1
        if t.startswith("@"):
            fails.append({"input": t, "status": "unknown", "class": None, "why": "a response file: not resolved"})
        elif t == "-Xlinker" and i < len(toks):
            flag(toks[i])
            i += 1
        elif t.startswith("-Wl,"):
            for f in t[4:].split(","):
                flag(f)
        elif t == "-Xclang" and i < len(toks):
            nxt = toks[i]
            i += 1
            m = re.match(r"--dependent-lib=(\S+)", nxt)
            if m:
                items.append((m.group(1) if m.group(1).lower().endswith(".lib") else m.group(1) + ".lib",
                              "dependent-lib"))
            elif not re.fullmatch(r"-\S+", nxt):
                fails.append({"input": "-Xclang " + nxt, "status": "unknown", "class": None, "why": "unknown flag"})
        elif t.startswith("-l"):
            items.append((t[2:] if t.lower().endswith(".lib") else t[2:] + ".lib", "-l"))
        elif t.startswith("-L"):
            paths.append(t[2:])
        elif re.match(r"/[A-Za-z][A-Za-z0-9_]*(:|$)", t):
            flag(t)  # a raw lld-link / lib flag
        elif t.lower().endswith(LINK_INPUT_EXT):
            items.append((t, "path"))
        elif any(re.fullmatch(rx, t) for rx in DRIVER_FLAG_RES):
            continue
        else:
            fails.append({"input": t, "status": "unknown", "class": None, "why": "unknown link-line token"})
    return items, fails, delay, paths


def auto_root(edges):
    for out, e in edges.items():
        if out.endswith("/app/main.cpp.obj") and e["rule"].startswith(COMPILE_RULE_PREFIXES) and e["explicit"]:
            src = norm(e["explicit"][0])
            if src.endswith("/app/main.cpp"):
                return src[: -len("/app/main.cpp")]
    return ""


# ------------------------------------------------------------------------------------------ the manifest

def rule_id(r):
    return r.get("rule_from") or r.get("path") or r.get("glob") or r.get("name")


class Manifest:
    def __init__(self, data):
        self.data = data
        self.classes = data.get("classes", {})

    def rules(self, section):
        s = self.data.get(section, [])
        return s.get("rules", []) if isinstance(s, dict) else s

    def rule(self, section, path):
        low = os.path.basename(path).lower()
        for r in self.rules(section):
            if ("path" in r and r["path"] == path) or ("glob" in r and fnmatch.fnmatchcase(path, r["glob"])) or \
                    ("name" in r and r["name"].lower() == low):
                return r
        return None

    def sibling(self, path):
        """The EXACT `path` sources rule of a real header's same-named .cpp (never a glob, never .inc / .inl)."""
        if not path.lower().endswith(REAL_HEADER_EXT):
            return None
        want = os.path.splitext(path)[0] + ".cpp"
        return next((r for r in self.rules("sources") if r.get("path") == want), None)

    def classify(self, section, path, oracle_off=False):
        r = self.rule(section, path)
        if r is None and section == "content":
            if path.lower().endswith(SOURCE_EXT):
                own = self.rule("sources", path)  # a source file read / incorporated as content: its own rule
                if own is not None:
                    r = dict(own, rule_from="sources " + rule_id(own))
            else:
                sib = self.sibling(path)
                if sib is not None:
                    sid = "sibling " + sib["path"]
                    if "header" in sib:  # a mixed unit's header names its own class (usually the shipping API)
                        r = {"class": sib["header"], "why": "the header of " + sib["path"], "rule_from": sid + " (header)"}
                    elif sib["class"] != "mixed":  # a mixed unit's header needs that rule's `header` class
                        r = dict(sib, rule_from=sid)
        if r is None:
            return {"class": None, "status": "unknown", "why": "no manifest rule", "part": "", "rule": None,
                    "system": False, "obj": None}
        cls = r.get("off", r["class"]) if oracle_off else r["class"]
        return {"class": cls, "status": self.classes.get(cls, "unknown-class"), "why": r.get("why", ""),
                "part": r.get("part", ""), "rule": rule_id(r), "system": bool(r.get("system")), "obj": r}

    def markers(self):
        """[(marker, rule id)] of the rules whose class is excluded (a marker may end in NUL: an exact C string)."""
        out = []
        for section in ("sources", "content", "embedded", "link_inputs"):
            for r in self.rules(section):
                if self.classes.get(r["class"]) == "exclude":
                    for m in r.get("markers", []):
                        out.append((m, rule_id(r)))
        return out

    def dynamic(self):
        s = self.data.get("runtime_dlls", [])
        return s.get("dynamic", []) if isinstance(s, dict) else []


# ------------------------------------------------------------------------------------------ the audit

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def is_link(path):
    """A symlink or a Windows junction / other reparse point (never followed)."""
    try:
        st = os.lstat(path)
    except OSError:
        return False
    return stat.S_ISLNK(st.st_mode) or bool(getattr(st, "st_file_attributes", 0) & 0x400)


def audit(args):
    manifest = Manifest(json.load(open(args.manifest, encoding="utf-8")))
    edges, workdir = parse_ninja(open(args.ninja, encoding="utf-8", errors="replace").read())
    root = norm(args.source_root) or auto_root(edges)
    g = Graph(edges, workdir or norm(os.path.dirname(os.path.abspath(args.ninja))), root)
    units, dlls, libs, externals, linkers, link_fails, delays, steps = g.closure(args.target)
    if not units:
        raise ValueError("target %r links no compiled unit" % args.target)
    srcdir = norm(args.source_dir) if args.source_dir else root
    builddir = norm(args.build_dir) if args.build_dir else norm(os.path.dirname(os.path.abspath(args.ninja)))
    incomplete, failing, produced_rows, rc_system = [], [], [], []

    def fail(item, kind):
        failing.append(dict(item, kind=kind))

    def local(rel):
        """Where a classified path is readable now (None: an absolute path outside both trees)."""
        if rel.startswith("<build>/"):
            return builddir + "/" + rel[len("<build>/"):]
        return rel if is_abs(rel) else srcdir + "/" + rel

    # generated items: what their producing edge incorporates
    prod_seen = set()

    def incorporate(ri, node, into, depth):
        c = manifest.classify("content", ri)
        produced_rows.append({"item": into, "input": ri, "class": c["class"]})
        if c["status"] in ("unknown", "unknown-class"):
            fail({"content": ri, "in": [into], "class": None, "status": "unknown",
                  "why": "material incorporated into %s with no manifest rule" % into}, "generated-input")
        elif c["status"] == "tool":
            fail({"content": ri, "in": [into], "class": c["class"], "status": "unknown",
                  "why": "declared as incorporated into %s but classified %s" % (into, c["class"])}, "generated-input")
        elif c["status"] not in RETAINED:
            fail({"content": ri, "in": [into], "class": c["class"], "status": c["status"],
                  "why": "excluded material incorporated into %s: %s" % (into, c["why"])}, "generated-input")
        if g.generated(node):
            production(ri, node, c["obj"], depth + 1)

    text_cache = {}

    def read_text(r):
        if r not in text_cache:
            try:
                with open(local(r), "rb") as fh:
                    text_cache[r] = decode_source(fh.read())
                if text_cache[r] is None:
                    incomplete.append("source: %s holds NUL bytes and no byte-order mark - undecodable" % r)
            except (OSError, TypeError):
                text_cache[r] = None
        return text_cache[r]

    def gpath(p):
        """A path in the build graph's own space (absolute, . and .. resolved): what the compiler sees. `p` is
        classified (`<build>/x`, root-relative) or absolute."""
        return norm(os.path.normpath(g.absolute(g.node_of(p)))).rstrip("/")

    def gdir(r):
        """The graph-space folder of a classified file."""
        return gpath(g.node_of(r)).rsplit("/", 1)[0]

    def candidates(name, dirs):
        """Every existing file an include / payload name can denote from the given graph-space folders, classified
        (resolved in graph space, read where it is readable)."""
        n = norm(name)
        out = []
        for q in ([n] if is_abs(n) else [d + "/" + n for d in dirs]):
            r = g.rel(norm(os.path.normpath(q)))
            lp = local(r)
            if lp and os.path.isfile(lp):
                out.append(r)
        return list(dict.fromkeys(out))

    exemptions, exempted = manifest.data.get("unresolved_includes", []), []

    def include_closure(start, search, owner, names=(), system=None, extra=None):
        """The include closure of incorporated text sources -> ([(file, includer)] beyond `start`, [(name, includer,
        path)] resolved by `system`). A sound over-approximation of every compiler's search, not a model of one:
        every include of every closure file (source_includes: all conditional blocks, comments read both ways) and
        every name in `names` ((form, name, includer): forced includes) is resolved against EVERY candidate folder -
        `search` (graph-space folders: the include search folders and the working folders) and the folder of every
        closure file (the includer's, its ancestors' and #include_next's next folders among them) - and EVERY existing
        candidate joins the closure, to a fixpoint. A name with no candidate, an unsupported form (#include MACRO)
        or an include_alias pragma is INCOMPLETE, unless `system(name)` resolves it outside the trees (an .rc's SDK
        headers from its compile's deps) or the manifest's `unresolved_includes` exempts that exact name from that
        exact includer (a name with no candidate in the trees only: an exemption never hides a tree file).
        `extra(text)` adds (form, name) includes a file states otherwise (an .rc's RCINCLUDE)."""
        files, out, seen = list(start), [], {x.lower() for x in start}
        dirs = list(dict.fromkeys(list(search) + [gdir(x) for x in start]))
        pending, k = list(names), 0
        changed = True
        while changed:
            changed = False
            while k < len(files):
                r = files[k]
                k += 1
                text = read_text(r)
                if text is None:
                    incomplete.append("include: %s (incorporated into %s) unreadable at %s" % (r, owner, local(r)))
                    continue
                pending += [(f, n, r) for f, n in source_includes(text)]
                if extra:
                    pending += [(f, n, r) for f, n in extra(text)]
            for form, name, frm in list(dict.fromkeys(pending)):
                if form in ("other", "alias"):
                    continue
                for c in candidates(name, dirs):  # loose (quote* / angle*) hits included
                    if c.lower() not in seen:
                        seen.add(c.lower())
                        files.append(c)
                        out.append((c, frm))
                        d = gdir(c)
                        if d not in dirs:
                            dirs.append(d)
                        changed = True
        sys_hits = []
        for form, name, frm in dict.fromkeys(pending):
            if form.endswith("*") or (form in ("quote", "angle") and candidates(name, dirs)):
                continue
            if form == "alias":
                incomplete.append("include: %r in %s (incorporated into %s): include_alias remaps include names - "
                                  "unsupported" % (name, frm, owner))
                continue
            s = system(name) if system is not None and form != "other" else None
            if s:
                sys_hits.append((name, frm, s))
                continue
            ex = next((x for x in exemptions if x.get("from") == frm and
                       (x.get("text") == name if form == "other" else x.get("name") == name)), None)
            if ex is not None:
                exempted.append({"include": name, "from": frm, "into": owner, "why": ex.get("why")})
            elif form == "other":
                incomplete.append("include: an unsupported form %r in %s (incorporated into %s)" % (name, frm, owner))
            else:
                incomplete.append("include: %s from %s (incorporated into %s) is in no candidate folder (the search "
                                  "and working folders, every closure file's folder)" % (
                                      '"%s"' % name if form == "quote" else "<%s>" % name, frm, owner))
        return out, sys_hits

    def production(item, node, rule, depth=0):
        if item in prod_seen:  # cycles and shared inputs: each item once
            return
        if depth > 200:
            incomplete.append("generated input %s: the producing-edge chain is deeper than 200 - not followed" % item)
            return
        prod_seen.add(item)
        gen = g.generated(node)
        if gen is None:
            return
        rid = rule_id(rule) if rule else None
        declared = list(rule.get("inputs", [])) if rule else []
        prereq = list(rule.get("prerequisites", [])) if rule else []
        kind, e = gen
        if kind == "build":
            if not rule or not rule.get("produced"):
                fail({"content": item, "in": [], "class": rule and rule["class"], "status": "unknown",
                      "why": "a build-directory file no build edge produces (a configure / link-step output): its "
                             "rule (%s) must declare `produced`" % rid}, "generated-input")
                return
            cfg = [g.rel(i) for i in (g.configure["explicit"] + g.configure["implicit"])] if g.configure else []
            for gl in declared:
                hit = [x for x in cfg if fnmatch.fnmatchcase(x, gl)]
                if not hit:
                    fail({"content": item, "in": [], "class": None, "status": "unknown",
                          "why": "declared input %s is not an input of the configure (RERUN_CMAKE)" % gl},
                         "generated-input")
                for x in hit:
                    incorporate(x, g.node_of(x), item, depth)
            return
        if e["rule"] == "phony":  # an alias: its inputs are the item
            for i in e["explicit"] + e["implicit"]:
                production(g.rel(i), i, rule, depth + 1)
            return
        raw = norm(e["vars"].get("COMMAND", ""))
        cmd = raw.lower()
        refs = command_refs(raw)
        # the working folders (graph space): the build dir and every `cd` operand
        wd = gpath("<build>/.")
        cwds = [wd] + [gpath(op) if is_abs(op) else norm(os.path.normpath(wd + "/" + op)) for op in
                       (norm(m.group(2).strip('"')) for m in CD_RE.finditer(raw))]
        matched, incorporated = set(), []
        for i, how in [(x, "in") for x in e["explicit"] + e["implicit"]] + [(x, "order") for x in e["order_only"]]:
            ri = g.rel(i)
            named = command_names(refs, norm(os.path.normpath(g.absolute(i))))
            hit = [gl for gl in declared if fnmatch.fnmatchcase(ri, gl)]
            if hit:
                matched.update(hit)
                incorporated.append((ri, i))
                continue
            if not named and (how == "order" or any(fnmatch.fnmatchcase(ri, gl) for gl in prereq)):
                continue
            c = manifest.classify("content", ri)
            if c["class"] == "build-tool" or (c["class"] == "prerequisite" and not named):
                continue
            fail({"content": ri, "in": [item], "class": c["class"], "status": "unknown",
                  "why": "an undeclared input of %s's producing edge%s: declare it in the rule's `inputs` or classify "
                         "it build-tool / prerequisite" % (item, " (named in its COMMAND)" if named else "")},
                 "generated-input")
        # a tree file the COMMAND names that is not on its edge: a build tool, or declared material
        on_edge = {norm(os.path.normpath(g.absolute(x))).lower()
                   for x in e["outputs"] + e["explicit"] + e["implicit"] + e["order_only"]}
        named_files = {g.rel(a) for a in refs[4]} | {g.rel(norm(os.path.normpath(d + "/" + tok)))
                                                      for tok in refs[3] for d in cwds}
        for ri in sorted(named_files):
            if is_abs(ri) or not os.path.isfile(local(ri)) or gpath(ri).lower() in on_edge:
                continue
            c = manifest.classify("content", ri)
            hit = [gl for gl in declared if fnmatch.fnmatchcase(ri, gl)]
            if hit:
                matched.update(hit)
                incorporated.append((ri, g.node_of(ri)))
            elif c["class"] != "build-tool":
                fail({"content": ri, "in": [item], "class": c["class"], "status": "unknown",
                      "why": "named in the COMMAND of %s's producing edge but not an input of the edge: declare it in "
                             "the rule's `inputs` or classify it build-tool" % item}, "generated-input")
        if PREPROCESSOR_RE.search(cmd):
            search = []
            for m in SEARCH_FLAG_RE.finditer(raw):
                op = norm(m.group(1).strip('"'))
                search += [gpath(op)] if is_abs(op) else [norm(os.path.normpath(d + "/" + op)) for d in cwds]
            forced = [("quote", norm(m.group(1).strip('"')), "<COMMAND of %s>" % item) for m in FORCED_FLAG_RE.finditer(raw)]
            closure, _ = include_closure([ri for ri, _ in incorporated], search + cwds, item, names=forced)
            for f, frm in closure:
                hit = [gl for gl in declared if fnmatch.fnmatchcase(f, gl)]
                if not hit:
                    fail({"content": f, "in": [item], "class": None, "status": "unknown",
                          "why": "incorporated into %s through an include from %s but not declared in its rule's "
                                 "`inputs`" % (item, frm)}, "generated-input")
                    continue
                matched.update(hit)
                incorporated.append((f, g.node_of(f)))
        for gl in declared:
            if gl not in matched:
                fail({"content": item, "in": [], "class": None, "status": "unknown",
                      "why": "declared input %s is not on its producing edge (nor in its #include closure)" % gl},
                     "generated-input")
        for ri, i in incorporated:
            incorporate(ri, i, item, depth)

    # 1 units (+ generated units' production)
    for u in units:
        u.update(manifest.classify("sources", u["source"], u["oracle_off"]))
        if u["status"] not in RETAINED:
            fail({k: u[k] for k in ("source", "component", "class", "status", "why", "part")}, "unit")
        elif g.generated(u["src_node"]):
            production(u["source"], u["src_node"], u["obj"])

    # link inputs, link-line failures, link steps
    for f in link_fails:
        fail(f, "link")
    for s in steps:
        norm_step = re.sub(re.escape(g.workdir), "<build>", norm(s["step"]), flags=re.I)
        if g.root:
            norm_step = re.sub(re.escape(g.root), "<src>", norm_step, flags=re.I)
        r = next((x for x in manifest.rules("link_steps") if fnmatch.fnmatchcase(norm_step, x["glob"])), None)
        s["normalized"] = norm_step
        if r is None:
            fail({"input": "%s %s: %s" % (s["edge"], s["var"], norm_step[:160]), "class": None, "status": "unknown",
                  "why": "an unclassified link step (no link_steps rule)"}, "link")
    link_rows, seen = [], set()
    for x in externals:
        tok = x["node"]
        key = g.rel(tok) if "/" in norm(tok) or x["generated"] else tok  # a bare name stays a name (searched)
        if key in seen:
            continue
        seen.add(key)
        c = manifest.classify("link_inputs", key)
        link_rows.append(dict({"input": key, "via": x["via"], "generated": x["generated"]},
                              **{k: c[k] for k in ("class", "status", "why", "rule")}))
        if c["status"] not in RETAINED:
            fail(dict({"input": key}, **{k: c[k] for k in ("class", "status", "why")}), "link")
        elif (x["generated"] or "/" in norm(tok)) and g.generated(tok):
            production(key, tok, c["obj"])

    # 2 content
    content = {}
    deps_info = None
    if args.deps:
        deps = parse_deps(args.deps)
        missing = [u["object"] for u in units if u["object"] not in deps]
        stale = [u["object"] for u in units if u["object"] in deps and not deps[u["object"]]["valid"]]
        miscount = [u["object"] for u in units if u["object"] in deps and
                    deps[u["object"]]["declared"] != len(deps[u["object"]]["deps"])]
        deps_info = {"records": len(deps), "linked_units": len(units), "missing": len(missing), "stale": len(stale),
                     "miscounted": len(miscount)}
        if missing:
            incomplete.append("deps: %d linked units have no ninja deps record (e.g. %s)" % (len(missing), missing[0]))
        if stale:
            incomplete.append("deps: %d linked units' deps are STALE (e.g. %s)" % (len(stale), stale[0]))
        if miscount:
            d = deps[miscount[0]]
            incomplete.append("deps: %d records' '#deps N' count differs from the paths listed (e.g. %s: declares "
                              "%s, lists %d)" % (len(miscount), miscount[0], d["declared"], len(d["deps"])))

        def note(r, u):
            v = content.setdefault(r, {"in": set(), "retained_in": set(), "all_off": True})
            v["in"].add(u["source"])
            v["all_off"] = v["all_off"] and u["oracle_off"]
            if u["status"] in RETAINED:
                v["retained_in"].add(u["source"])

        for u in units:
            for d in deps.get(u["object"], {"deps": []})["deps"]:
                r = g.rel(d)
                if r != u["source"]:
                    note(r, u)
            if u["source"].lower().endswith(".rc"):
                rc_path = local(u["source"])
                try:
                    rc = open(rc_path, encoding="utf-8", errors="replace").read()
                except OSError:
                    incomplete.append("rc: %s unreadable under --source-dir %s" % (u["source"], srcdir))
                    continue
                # its include closure (#include and RCINCLUDE): each file it reaches is content AND is parsed for
                # resource statements (fail-closed: rc.exe reads only the directives of a .h / .c, other resource
                # compilers need not); a name outside the trees must be in the compile's own deps (an SDK header).
                e = g.edge(u["object"])
                raw = norm(" ".join(e["vars"].get(k, "") for k in ("DEFINES", "INCLUDES", "FLAGS")) if e else "")
                rc_dir, wd = gdir(u["source"]), gpath("<build>/.")
                search = [gpath(op) if is_abs(op) else norm(os.path.normpath(wd + "/" + op))
                          for op in (norm(m.group(1).strip('"')) for m in SEARCH_FLAG_RE.finditer(raw))] + [wd]
                rdeps = [norm(d) for d in deps.get(u["object"], {"deps": []})["deps"]]
                n_inc, n_ex = len(incomplete), len(exempted)

                def sdk(name, rdeps=rdeps):
                    tail = "/" + norm(name).lower().lstrip("/")
                    return next((d for d in rdeps if d.lower().endswith(tail) and is_abs(g.rel(d))), None)

                macros = {}

                def rcinc(text, macros=macros):  # RCINCLUDE operands, the macros found so far expanded
                    return [("quote", f) for f in rc_resources(text, macros)[1]]
                for it in range(8):  # the closure and its macros to a fixpoint (a macro can name an RCINCLUDE)
                    if it:  # the pass is redone: its reports again
                        del incomplete[n_inc:]
                        del exempted[n_ex:]
                        text_cache.clear()
                    closure, sys_hits = include_closure([u["source"]], search, u["source"], system=sdk, extra=rcinc)
                    found = {}
                    for m in re.finditer(r'(?:^|\s)[/-]D\s*([A-Za-z_]\w*)(?:=("[^"]*"|\S*))?', raw):
                        found.setdefault(m.group(1), []).append(("", m.group(2) if m.group(2) is not None else "1"))
                    for f in [u["source"]] + [c for c, _ in closure]:
                        for txt in pp_texts(read_text(f) or "")[1:2]:  # spliced, comments removed
                            for m in re.finditer(r"^[^\S\n]*(?:#|%:)[^\S\n]*define[^\S\n]+([A-Za-z_]\w*)"
                                                 r"(\([^)]*\))?[^\S\n]*(.*)$", txt, re.M):
                                found.setdefault(m.group(1), []).append((m.group(2) or "", m.group(3).strip()))
                    if found == macros:
                        break
                    macros.clear()
                    macros.update(found)
                else:
                    incomplete.append("rc: %s: its macros and RCINCLUDE closure did not settle" % u["source"])
                for f, frm in [(u["source"], None)] + closure:
                    if frm is not None:
                        note(f, u)
                    sub = read_text(f)
                    if sub is None:
                        continue  # include_closure recorded it
                    files, _, problems = rc_resources(sub, macros)
                    for p in problems:
                        incomplete.append("rc: %s%s: %s" % (f, " (included by %s)" % frm if frm else "", p))
                    # a payload: every existing candidate (the .rc's folder, the stating file's, the search
                    # folders, the build dir) - else the .rc-relative spelling
                    for x in files:
                        cands = candidates(x, [rc_dir, gdir(f)] + search)
                        for c in cands or [g.rel(norm(os.path.normpath(x if is_abs(x) else rc_dir + "/" + x)))]:
                            note(c, u)
                rc_system.extend({"rc": u["source"], "include": n, "from": frm, "path": s} for n, frm, s in sys_hits)
    else:
        incomplete.append("deps: not given (--deps): the content closure is unaudited")
    content_rows = []
    # Every reader is recorded (`in` complete; the text report truncates for display). A list cut to five hid a sixth
    # retained reader of an excluded header from an audit-to-audit comparison keyed on it (Astra round 2 on
    # release/emu-removal stage 2a): the dependency a regression adds must be an item of its own.
    for r, v in sorted(content.items()):
        c = manifest.classify("content", r, oracle_off=v["all_off"])
        row = dict({"content": r, "in": sorted(v["in"]), "in_count": len(v["in"])},
                   **{k: c[k] for k in ("class", "status", "why", "part", "rule")})
        content_rows.append(row)
        if c["status"] in ("unknown", "unknown-class"):
            fail({"content": r, "in": row["in"], "class": None, "status": "unknown", "why": "no manifest rule"},
                 "content")
        elif c["status"] == "tool":
            fail({"content": r, "in": row["in"], "class": c["class"], "status": "unknown",
                  "why": "classified %s but read by a compile" % c["class"]}, "content")
        elif c["status"] == "exclude" and v["retained_in"]:
            fail({"content": r, "in": sorted(v["retained_in"]), "class": c["class"], "status": "exclude",
                  "why": "excluded content read by a retained unit: " + c["why"]}, "content")
        if v["retained_in"] and c["status"] in RETAINED and g.generated(g.node_of(r)):
            production(r, g.node_of(r), c["obj"])

    # exe, PDB, modules
    exe_info, pdb_info, modules, pdb_evidence, exe = None, None, [], {}, None
    if args.exe:
        exe = PE(args.exe)
        exe_info = {"imports": exe.imports(), "delay_imports": exe.delay_imports(), "codeview": exe.codeview(),
                    "sha256": hashlib.sha256(exe.data).hexdigest()}
    else:
        incomplete.append("exe: not given (--exe)")
    dbi = None
    if args.pdb:
        try:
            dbi = read_pdb(args.pdb)
            pdb_evidence["dbi_modules"] = len(dbi["modules"])
        except (OSError, ValueError, struct.error) as e:
            incomplete.append("pdb: the PDB's own module list is unreadable (%s)" % e)
    mods_text, dump_mods = None, None
    if args.modules:
        mods_text = open(args.modules, encoding="utf-8", errors="replace").read()
        pdb_evidence["dump"] = norm(args.modules)
    elif args.pdb:
        tool = pdbutil_path(args.pdbutil)
        if tool is not None:
            rc, mods_text = run_pdbutil(tool, args.pdb)
            pdb_evidence["pdbutil_exit"] = rc
            if rc != 0:
                incomplete.append("pdb: llvm-pdbutil exited %d: its module dump is not evidence" % rc)
    if not args.pdb and not args.modules:
        incomplete.append("pdb: not given (--pdb): what survived linking is unaudited")
    elif not args.pdb:
        incomplete.append("pdb: only a saved module dump (--modules), no PDB (--pdb): its completeness cannot be "
                          "checked against the PDB's DBI stream")
    if mods_text is not None:
        dump_mods, problems = parse_pdb_modules(mods_text)
        pdb_evidence["dump_modules"] = len(dump_mods)
        for p in problems:
            incomplete.append("pdb: the module dump is not complete (%s)" % p)
        summ = parse_pdb_summary(mods_text)
        if dbi is not None:
            if [(norm(a), norm(b)) for a, b in dump_mods] != [(norm(a), norm(b)) for a, b in dbi["modules"]]:
                incomplete.append("pdb: the module dump (%d modules) disagrees with the PDB's DBI stream (%d)" % (
                    len(dump_mods), len(dbi["modules"])))
            if summ is not None and (summ["guid"] != dbi["guid"] or summ["age"] != dbi["age"]):
                incomplete.append("pdb: the dump's summary GUID / age is not the PDB's")
        pdb_info = summ
    if dbi is not None:
        pdb_info = {"guid": dbi["guid"], "age": dbi["age"]}
    mods = dbi["modules"] if dbi is not None else (dump_mods or [])
    if (args.pdb or args.modules) and pdb_info is None:
        incomplete.append("pdb: no GUID: the PDB is not bound to the exe")
    elif pdb_info is not None and exe_info is not None:
        cv = exe_info["codeview"] or {}
        if not cv or cv.get("guid", "").upper() != pdb_info["guid"] or cv.get("age") != pdb_info["age"]:
            fail({"file": norm(args.pdb or args.modules), "class": None, "status": "unbound",
                  "why": "the PDB is not the exe's (CodeView %s age %s vs PDB %s age %s)" % (
                      cv.get("guid"), cv.get("age"), pdb_info["guid"], pdb_info["age"])}, "pdb")
    if (args.pdb or args.modules) and not mods:
        incomplete.append("pdb: no modules read")
    def graph_abs(p):
        return norm(os.path.normpath(g.absolute(p))).lower()

    objs = {graph_abs(u["object"]): u for u in units}
    lib_members = {}
    for u in units:
        if u["lib"]:
            lib_members.setdefault(graph_abs(u["lib"]), set()).add(os.path.basename(u["object"]).lower())
    import_libs = {graph_abs(lib) for d in dlls.values() for lib in d["import_libs"]}
    has_rc = any(u["source"].lower().endswith(".rc") for u in units)

    def module_input(p):
        """A PDB module's object / library path. lld writes it absolute, or - linked with /pdbsourcepath:.
        (step4/repro, SR_REPRODUCIBLE) - as ".\\<path relative to the build directory the link ran in>"."""
        q = norm(p)
        if not q:
            return q
        if is_abs(q):
            return norm(os.path.normpath(q))
        return norm(os.path.normpath(g.absolute(q[2:] if q.startswith("./") else q)))

    for name, obj in mods:
        obj = module_input(obj)
        o = obj.lower()
        if name == "* Linker *" or (obj.endswith("internal .obj file created from .res files") and has_rc):
            continue
        if o in objs:
            continue
        if o in lib_members:
            if norm(name).lower().split("/")[-1] not in lib_members[o]:
                fail({"module": name, "input": norm(obj), "class": None, "status": "unknown",
                      "why": "a member of a closure library that is no closure unit"}, "module")
            continue
        if o in import_libs:
            continue
        c = manifest.classify("link_inputs", g.rel(obj))
        modules.append({"module": name, "input": g.rel(obj), "class": c["class"]})
        if c["status"] not in RETAINED:
            fail({"module": name, "input": g.rel(obj), "class": c["class"], "status": c["status"],
                  "why": c["why"] or "a linked module from an unclassified input"}, "module")

    # package
    pkg_rows, pkg_files, pkg_links, staged = [], {}, [], {}
    if args.package:
        if not os.path.isdir(args.package):
            raise ValueError("package %r is not a directory" % args.package)
        for dp, dns, fs in os.walk(args.package):
            for d in list(dns):
                if is_link(os.path.join(dp, d)):
                    pkg_links.append(norm(os.path.relpath(os.path.join(dp, d), args.package)))
                    dns.remove(d)  # never followed
            for f in fs:
                p = os.path.join(dp, f)
                r = norm(os.path.relpath(p, args.package))
                if is_link(p):
                    pkg_links.append(r)
                else:
                    pkg_files[r] = p
        if not pkg_files:
            raise ValueError("package %r is empty" % args.package)
        for r in sorted(pkg_links):
            c = manifest.classify("package", r)
            ok = c["status"] in RETAINED and c["obj"] is not None and c["obj"].get("link")
            pkg_rows.append(dict({"file": r, "link": True}, **{k: c[k] for k in ("class", "status", "why", "rule")}))
            if not ok:
                fail({"file": r, "class": c["class"], "status": "unknown",
                      "why": "a link / junction in the package (not followed): it needs a package rule with "
                             "\"link\": true"}, "package")
        for r in sorted(pkg_files):
            c = manifest.classify("package", r)
            if r.lower().endswith(EXEC_EXT) and c["obj"] is not None and "path" not in c["obj"]:
                c = dict(c, status="unknown", why="an executable file needs an exact path rule (got %s)" % c["rule"])
            if c["obj"] is not None and c["obj"].get("link"):
                c = dict(c, status="unknown", why="a link rule matched a regular file")
            pkg_rows.append(dict({"file": r}, **{k: c[k] for k in ("class", "status", "why", "rule")}))
            if c["status"] not in RETAINED:
                fail(dict({"file": r}, **{k: c[k] for k in ("class", "status", "why")}), "package")
        exe_name = os.path.basename(args.exe) if args.exe else args.target
        sp = next((p for r, p in pkg_files.items() if os.path.basename(r).lower() == exe_name.lower()), None)
        if sp is None:
            fail({"file": exe_name, "class": None, "status": "unbound", "why": "the package has no target exe"},
                 "package")
        elif exe is not None:
            data = open(sp, "rb").read()
            if data == exe.data:
                staged["exe"] = "identical"
            elif same_but_subsystem(data, exe.data):
                staged["exe"] = "identical but the PE Subsystem field (%d -> %d)" % (
                    struct.unpack_from("<H", exe.data, exe.subsystem_off)[0],
                    struct.unpack_from("<H", data, exe.subsystem_off)[0])
            else:
                fail({"file": exe_name, "class": None, "status": "unbound",
                      "why": "the package's exe is not the audited exe"}, "package")
        # the packager's manifest
        mpath = pkg_files.get("manifest.json")
        pm = None
        if mpath is not None:
            try:
                pm = json.load(open(mpath, encoding="utf-8-sig"))
            except (OSError, ValueError):
                pm = None
        if not isinstance(pm, dict) or (pm.get("packager") or {}).get("script") != PACKAGER:
            incomplete.append("package: not staged by %s (no packager manifest.json): a substitute package" % PACKAGER)
            staged["by"] = None
        else:
            staged["by"] = pm["packager"]
            listed = pm.get("files") or {}
            actual = {r: sha256(p) for r, p in pkg_files.items() if r != "manifest.json"}
            extra, gone = sorted(set(actual) - set(listed)), sorted(set(listed) - set(actual))
            changed = sorted(r for r in set(actual) & set(listed) if str(listed[r]).lower() != actual[r])
            if extra or gone or changed:
                fail({"file": "manifest.json", "class": None, "status": "unbound",
                      "why": "the package differs from its packager manifest: %d files not listed (%s), %d listed "
                             "missing (%s), %d changed since staging (%s)" % (
                                 len(extra), ", ".join(extra[:3]), len(gone), ", ".join(gone[:3]), len(changed),
                                 ", ".join(changed[:3]))}, "package")
            built = ((pm.get("source") or {}).get("build_exe_sha256") or "").lower()
            if exe_info is not None and built != exe_info["sha256"]:
                fail({"file": "manifest.json", "class": None, "status": "unbound",
                      "why": "the packager staged another build's exe (%s)" % (built[:16] or "none")}, "package")
    else:
        incomplete.append("package: not given (--package)")

    # runtime DLLs: imports, dynamic loads, binding, delay loads, markers
    dll_rows, markers_found, binding = [], [], []
    staged_lower = {os.path.basename(r).lower(): p for r, p in pkg_files.items()}
    pes = {}
    if exe is not None:
        pes[os.path.basename(args.exe)] = exe
    for r in sorted(pkg_files):
        if r.lower().endswith(".dll"):
            try:
                pes[r] = PE(pkg_files[r])
            except ValueError as e:
                fail({"file": r, "class": None, "status": "unknown", "why": str(e)}, "dll")

    def need(dll, by, how):
        c = manifest.classify("runtime_dlls", dll)
        dll_rows.append(dict({"dll": dll, "by": by, "how": how}, **{k: c[k] for k in ("class", "status", "rule")}))
        if c["status"] not in RETAINED:
            fail(dict({"dll": dll, "in": [by]}, **{k: c[k] for k in ("class", "status", "why")}), "dll")
        elif not c["system"] and args.package and dll.lower() not in staged_lower:
            fail({"dll": dll, "in": [by], "class": c["class"], "status": "unresolved",
                  "why": "neither a system DLL nor staged in the package (%s)" % how}, "dll")

    for name, pe in pes.items():
        for dll, how in [(x, "import") for x in pe.imports()] + [(x, "delay") for x in pe.delay_imports()]:
            need(dll, name, how)
        for m, rid in manifest.markers():
            n = pe.data.count(m.encode("latin-1"))
            if n:
                markers_found.append({"string": m.replace("\0", "\\0"), "rule": rid, "in": name, "count": n})
    linked_sources = {u["source"] for u in units}
    for d in manifest.dynamic():
        if d.get("by") and d["by"] not in linked_sources:
            dll_rows.append({"dll": d["dll"], "by": d["by"], "how": "dynamic (loader not linked)", "class": None})
            continue
        need(d["dll"], d.get("by", "?"), "dynamic: " + d.get("how", "LoadLibrary"))
    for d in delays:
        binary = os.path.basename(args.exe) if (d["kind"] == "exe" and args.exe) else \
            next((r for r in pes if os.path.basename(r).lower() == d["component"].lower()), None)
        if binary is None or binary not in pes:
            if d["kind"] != "lib":
                incomplete.append("delayload: %s's /DELAYLOAD:%s cannot be checked (%s not audited)" % (
                    d["component"], d["dll"], d["component"]))
            continue
        if d["dll"].lower() not in [x.lower() for x in pes[binary].delay_imports()]:
            incomplete.append("delayload: /DELAYLOAD:%s on %s's link line is not in its delay imports" % (
                d["dll"], d["component"]))
    builds = {}
    for k, e in edges.items():
        for o in e["outputs"]:
            if o.lower().endswith(".dll"):
                builds.setdefault(os.path.basename(norm(o)).lower(), o)
    for r in sorted(pkg_files):
        if not r.lower().endswith(".dll"):
            continue
        name, have = os.path.basename(r), sha256(pkg_files[r])
        row = {"dll": r, "sha256": have}
        rule = manifest.rule("runtime_dlls", name)
        if name in dlls:
            out = dlls[name]["output"]
            bp = local(g.rel(out))
            row["bound_to"] = "build output " + g.rel(out)
            if not os.path.isfile(bp):
                row["verdict"] = "unverified"
                incomplete.append("dll: %s is the closure's build output %s, but the build dir's copy is not "
                                  "readable (%s; --build-dir)" % (name, g.rel(out), bp))
            elif sha256(bp) != have:
                row["verdict"] = "substituted"
                fail({"file": r, "class": None, "status": "unbound",
                      "why": "the packaged DLL is not the build's %s" % g.rel(out)}, "dll")
            else:
                row["verdict"] = "bound"
        elif name.lower() in builds:
            row["bound_to"], row["verdict"] = "build output outside the audited closure", "unbound"
            fail({"file": r, "class": None, "status": "unbound",
                  "why": "a build output (%s) the target does not link: its units are unaudited" % builds[
                      name.lower()]}, "dll")
        elif rule is not None and rule.get("sha256"):
            pins = [x.lower() for x in rule["sha256"]]
            row["bound_to"] = "vendor pin (%s)" % rule.get("vendor", "")
            row["verdict"] = "bound" if have in pins else "substituted"
            if have not in pins:
                fail({"file": r, "class": None, "status": "unbound",
                      "why": "not the pinned vendor artifact (sha256 %s)" % have[:16]}, "dll")
        else:
            row["bound_to"], row["verdict"] = None, "unbound"
            fail({"file": r, "class": None, "status": "unbound",
                  "why": "bound to no build output and no vendor sha256 pin"}, "dll")
        binding.append(row)
    for m in markers_found:
        fail({"marker": m["string"], "in": [m["in"]], "class": None, "status": "marker",
              "why": "a marker of the excluded %s occurs %d times" % (m["rule"], m["count"])}, "marker")

    verdict = "FAIL" if failing else ("INCOMPLETE" if incomplete else "PASS")
    counts = {}
    for u in units:
        counts[str(u["class"])] = counts.get(str(u["class"]), 0) + 1
    return {
        "schema": "composition_audit/3",
        "target": args.target, "ninja": norm(os.path.abspath(args.ninja)), "build_dir": workdir, "source_root": root,
        "readable": {"build_dir": builddir, "source_dir": srcdir},
        "evidence": {"deps": deps_info, "exe": bool(args.exe), "pdb": dict(pdb_evidence, given=bool(args.pdb),
                                                                        dump_given=bool(args.modules)),
                     "package": dict(staged, given=bool(args.package)), "rc_system_includes": rc_system,
                     "include_exemptions": exempted},
        "manifest": norm(os.path.relpath(args.manifest, REPO)) if os.path.abspath(args.manifest).startswith(REPO)
        else norm(args.manifest),
        "verdict": verdict, "incomplete": incomplete,
        "counts": dict(sorted(counts.items())), "units_total": len(units), "oracle_off_units": sum(
            1 for u in units if u["oracle_off"]),
        "dlls": {k: {"units": v["units"]} for k, v in dlls.items()},
        "linkers": [{"node": L["node"], "kind": L["kind"], "component": L["component"]} for L in linkers],
        "failing": failing,
        "unknown": sorted({f.get("source") or f.get("content") or f.get("input") or f.get("file") or f.get("dll")
                           or f.get("module") or "" for f in failing if f["status"] == "unknown"}),
        "review": [{"source": u["source"], "class": u["class"], "why": u["why"]} for u in units
                   if u["status"] == "review"],
        "exe": exe_info and {k: exe_info[k] for k in ("imports", "delay_imports", "codeview", "sha256")},
        "pdb": pdb_info, "markers_found": markers_found, "marker_count": len(manifest.markers()),
        "units": [{k: u.get(k) for k in ("object", "source", "component", "class", "status", "why", "part",
                                          "oracle_off") if u.get(k) not in (None, "", [])} for u in units],
        "link_inputs": link_rows, "link_steps": steps, "content": content_rows, "produced_from": produced_rows,
        "external_modules": modules, "runtime_dlls": dll_rows, "dll_binding": binding, "package_files": pkg_rows,
    }


def what(f):
    return f.get("source") or f.get("content") or f.get("module") or f.get("input") or f.get("file") or \
        f.get("dll") or f.get("marker") or "?"


def summary(res):
    lines = ["composition_audit: %s - target %s, %d linked units (%s)%s" % (
        res["verdict"], res["target"], res["units_total"], ", ".join("%s %d" % kv for kv in res["counts"].items()),
        (", %d compiled with SR_ORACLE=0" % res["oracle_off_units"]) if res["oracle_off_units"] else "")]
    for dll, d in sorted(res["dlls"].items()):
        lines.append("  dll %s: %d units" % (dll, d["units"]))
    for msg in res["incomplete"]:
        lines.append("  INCOMPLETE %s" % msg)
    by = {}
    for f in res["failing"]:
        by.setdefault((f["kind"], f["status"] if f["status"] in ("unknown", "unbound", "unresolved", "marker")
                       else f["class"]), []).append(f)
    for (kind, cls), fs in sorted(by.items(), key=lambda kv: (kv[0][0], str(kv[0][1]))):
        lines.append("  FAIL %s %s (%d):" % (kind, cls, len(fs)))
        groups = {}
        for f in fs:
            groups.setdefault(f.get("why", "") if cls not in ("unknown",) else what(f), []).append(f)
        for why, items in groups.items():
            ws = [what(f) for f in items]
            if len(ws) == 1:
                extra = (" (in %s)" % ", ".join(items[0]["in"][:3])) if items[0].get("in") and \
                    kind in ("content", "generated-input") else ""
                lines.append("    %s%s%s" % (ws[0], (" - " + why) if why and why != ws[0] else "", extra))
            else:
                lines.append("    %d items - %s: %s" % (len(ws), why, ", ".join(os.path.basename(w) for w in ws)))
    if res["review"]:
        lines.append("  review (%d, an owner decision pending): %s" % (
            len(res["review"]), ", ".join(r["source"] for r in res["review"])))
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ninja", required=True)
    ap.add_argument("--deps")
    ap.add_argument("--target", default="SupermanReturns.exe")
    ap.add_argument("--package")
    ap.add_argument("--exe")
    ap.add_argument("--pdb")
    ap.add_argument("--modules", help="a saved `llvm-pdbutil dump -summary -modules` of --pdb")
    ap.add_argument("--pdbutil")
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    ap.add_argument("--source-root", default="", help="the source root the build was configured from (auto)")
    ap.add_argument("--source-dir", default="", help="where that tree is readable now (default: --source-root)")
    ap.add_argument("--build-dir", default="", help="where the build tree's outputs are readable now (default: the "
                    "folder of --ninja)")
    ap.add_argument("--out")
    args = ap.parse_args(argv)
    try:
        res = audit(args)
    except (OSError, ValueError) as e:
        print("composition_audit: ERROR %s" % e, file=sys.stderr)
        return 2
    out = args.out or os.path.join(os.path.dirname(os.path.abspath(args.ninja)), "composition_audit.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1)
    print(summary(res))
    print("wrote %s" % norm(out))
    return 0 if res["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
