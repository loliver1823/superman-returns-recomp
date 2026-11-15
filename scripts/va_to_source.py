#!/usr/bin/env python3
"""
VA-to-source index: maps guest virtual addresses to ppc_recomp file:line:function.

Build the index (run once, re-run after XenonRecomp regenerates):
    python scripts/va_to_source.py --build

Lookup a guest VA:
    python scripts/va_to_source.py 0x824DB158
    python scripts/va_to_source.py 0x824DB158 0x82110058 ...

Start as MCP server (used by Claude Code automatically):
    python scripts/va_to_source.py --mcp
"""

import argparse
from contextlib import closing
import hashlib
import json
import os
import re
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).parent.parent
RECOMP_DIR = ROOT / "guest" / "ppc"
DB_PATH = ROOT / ".cache" / "va_index.db"

# IDA plugin RPC (mrexodia ida-pro-mcp, running inside the GUI IDA). We enrich
# each VA lookup with IDA's view (function name + Hex-Rays decompile) WHEN it's
# reachable, and degrade gracefully to recomp-only when it isn't. Override host
# with SR_IDA_RPC. This is the "IDA-backed address hub": guest VA -> the C we
# actually run (recomp file:line) + IDA's authoritative name/decompile.
IDA_RPC = os.environ.get("SR_IDA_RPC", "http://127.0.0.1:13337/mcp")


def _ida_rpc(method, params, timeout=6):
    """Call the IDA plugin's JSON-RPC. Returns the 'result' or None if IDA is down."""
    import urllib.request
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode()
    try:
        req = urllib.request.Request(IDA_RPC, data=body,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            obj = json.loads(resp.read().decode("utf-8", "replace"))
        return obj.get("result")
    except Exception:
        return None


def _ida_name_for(va):
    """IDA's function name for a guest VA (our ported ground truth), or None."""
    info = _ida_rpc("get_function_by_address", {"address": hex(va)})
    if isinstance(info, dict) and info.get("name"):
        return info["name"]
    return None

FUNC_RE = re.compile(
    r'PPC_FUNC_IMPL\s*\(\s*__imp__sub_([0-9A-Fa-f]+)\s*\)'
)
# Matches the generated loc_XXXXXXXX: labels used as branch targets inside functions
LOC_RE = re.compile(r'\bloc_([0-9A-Fa-f]{8})\s*:')


def source_fingerprint(src_dir: Path | None = None) -> str:
    """CONTENT digest of the generated-source inventory this index was built from.

    iter-455i, first attempt: this hashed only name|size|mtime_ns. The director defeated it
    with a same-size edit and a restored mtime - the fingerprint was IDENTICAL, so a stale
    index was ACCEPTED. The caveat shipped alongside it ("errs toward refusing") was simply
    wrong in that direction. Metadata is not a content check and must never be treated as
    one. A full content digest of the ~147 MiB tree measures ~0.2 s, so there was never a
    performance argument for the weaker version either.
    """
    src = src_dir or RECOMP_DIR
    h = hashlib.sha256()
    for cpp in sorted(src.glob("ppc_recomp.*.cpp")):
        h.update(cpp.name.encode())
        h.update(b"\0")
        with open(cpp, "rb") as f:
            while True:
                chunk = f.read(1 << 20)
                if not chunk:
                    break
                h.update(chunk)
        h.update(b"\n")
    return h.hexdigest()


def _stored_fingerprint(con) -> str | None:
    try:
        row = con.execute("SELECT v FROM meta WHERE k='source_fingerprint'").fetchone()
        return row[0] if row else None
    except sqlite3.Error:
        return None          # pre-repair database: no meta table at all


def check_fresh(con) -> dict | None:
    """Return an error dict if the index must not be trusted, else None (FAIL CLOSED)."""
    stored = _stored_fingerprint(con)
    if stored is None:
        return {"error": "va_index is from before the freshness repair (no fingerprint). "
                         "It cannot be trusted. Run: python scripts/va_to_source.py --build",
                "stale": True}
    current = source_fingerprint(RECOMP_DIR)
    if stored != current:
        return {"error": "va_index is STALE: guest/ppc has changed since it was built. "
                         "Refusing to answer. Run: python scripts/va_to_source.py --build",
                "stale": True, "stored_fingerprint": stored[:16],
                "current_fingerprint": current[:16]}
    return None


def build_index(db_path: Path | None = None, src_dir: Path | None = None,
                quiet: bool = False):
    """Build the index, then swap it in atomically.

    Guarantees (each one is a director finding against the first attempt):
      - the stored fingerprint is a CONTENT digest, taken BEFORE indexing;
      - a post-build digest must equal it, else the build ABORTS - closing the TOCTOU gap
        where sources changed mid-index and the result carried the final fingerprint;
      - the temp database is UNIQUE per builder in the same directory, and a builder must
        win an atomic O_EXCL lock, so concurrent builds are deterministic instead of
        colliding on one shared `.tmp`;
      - any failure or interruption leaves the PRIOR live database untouched and removes
        only this builder's own temp files.
    """
    db = db_path or DB_PATH
    src = src_dir or RECOMP_DIR
    if not src.is_dir() or not any(src.glob("ppc_recomp.*.cpp")):
        raise RuntimeError("no generated source files at %s; prior VA index is untouched" % src)
    db.parent.mkdir(parents=True, exist_ok=True)

    lock_path = db.with_name(db.name + ".buildlock")
    try:
        lock_fd = os.open(str(lock_path), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        raise RuntimeError(
            "another va_index build holds %s - refusing to build concurrently. "
            "If no build is running, delete that file." % lock_path)
    os.write(lock_fd, str(os.getpid()).encode())
    os.close(lock_fd)

    tmp_path = db.with_name("%s.%d.tmp" % (db.name, os.getpid()))
    con = None
    try:
        pre_digest = source_fingerprint(src)
        if tmp_path.exists():
            tmp_path.unlink()
        con = sqlite3.connect(tmp_path)
        con.execute("DROP TABLE IF EXISTS funcs")
        con.execute("""
            CREATE TABLE funcs (
                va       INTEGER PRIMARY KEY,
                file     TEXT NOT NULL,
                line     INTEGER NOT NULL,
                name     TEXT NOT NULL
            )
        """)
        con.execute("DROP TABLE IF EXISTS locs")
        con.execute("""
            CREATE TABLE locs (
                va       INTEGER NOT NULL,
                file     TEXT NOT NULL,
                line     INTEGER NOT NULL,
                func_va  INTEGER NOT NULL
            )
        """)

        cpp_files = sorted(src.glob("ppc_recomp.*.cpp"))
        total_funcs = 0
        total_locs = 0

        for cpp in cpp_files:
            try:
                rel = cpp.relative_to(ROOT).as_posix()
            except ValueError:
                rel = cpp.name          # hermetic self-test dirs live outside the repo
            current_func_va = None
            func_rows = []
            loc_rows = []

            with open(cpp, encoding="utf-8", errors="replace") as f:
                for lineno, raw in enumerate(f, 1):
                    fm = FUNC_RE.search(raw)
                    if fm:
                        va = int(fm.group(1), 16)
                        name = f"sub_{fm.group(1).upper()}"
                        func_rows.append((va, rel, lineno, name))
                        current_func_va = va
                        continue

                    if current_func_va is not None:
                        for lm in LOC_RE.finditer(raw):
                            va = int(lm.group(1), 16)
                            loc_rows.append((va, rel, lineno, current_func_va))

            con.executemany("INSERT OR REPLACE INTO funcs VALUES (?,?,?,?)", func_rows)
            con.executemany("INSERT OR REPLACE INTO locs VALUES (?,?,?,?)", loc_rows)
            total_funcs += len(func_rows)
            total_locs += len(loc_rows)
            if not quiet:
                print(f"  {rel}: {len(func_rows)} funcs, {len(loc_rows)} locs")

        # TOCTOU close: the sources must not have changed while we were reading them.
        post_digest = source_fingerprint(src)
        if post_digest != pre_digest:
            raise RuntimeError(
                "guest/ppc changed DURING the build (pre %s != post %s). Aborting so a "
                "mixed index cannot be published. The previous database is untouched."
                % (pre_digest[:16], post_digest[:16]))
        if not total_funcs:
            raise RuntimeError("generated source inventory contains no functions; prior VA index is untouched")

        con.execute("CREATE INDEX IF NOT EXISTS locs_va ON locs(va)")
        con.execute("CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
        con.execute("INSERT OR REPLACE INTO meta VALUES ('source_fingerprint', ?)",
                    (pre_digest,))
        con.execute("INSERT OR REPLACE INTO meta VALUES ('source_file_count', ?)",
                    (str(len(cpp_files)),))
        con.commit()
        con.close()
        con = None

        os.replace(tmp_path, db)        # atomic publish
        if not quiet:
            print(f"\nDone. {total_funcs} functions, {total_locs} branch labels indexed -> {db}")
            print(f"source_fingerprint: {pre_digest[:16]}  ({len(cpp_files)} files)")
        return {"funcs": total_funcs, "locs": total_locs, "fingerprint": pre_digest}
    finally:
        if con is not None:
            con.close()
        if tmp_path.exists():
            tmp_path.unlink()           # only ever this builder's own temp
        if lock_path.exists():
            lock_path.unlink()


def ensure_fresh_index(db_path: Path | None = None, src_dir: Path | None = None,
                       quiet: bool = True):
    """Refresh after source publication, retaining byte checks on the no-op path.

    Index existence/mtime is never generation identity. A corrupt or stale index
    is rebuilt atomically; a failed build leaves the previous database untouched.
    """
    db, src = db_path or DB_PATH, src_dir or RECOMP_DIR
    if not src.is_dir() or not any(src.glob("ppc_recomp.*.cpp")):
        raise RuntimeError("no generated source files at %s; refusing VA index refresh" % src)
    current = source_fingerprint(src)
    if db.is_file():
        try:
            with closing(sqlite3.connect(db.resolve().as_uri() + "?mode=ro", uri=True)) as con:
                if (_stored_fingerprint(con) == current and
                        con.execute("PRAGMA quick_check").fetchall() == [("ok",)] and
                        con.execute("SELECT va,file,line,name FROM funcs LIMIT 1").fetchone() is not None):
                    con.execute("SELECT va,file,line,func_va FROM locs LIMIT 0")
                    return {"status": "current", "fingerprint": current}
        except sqlite3.Error:
            pass
    return {"status": "rebuilt", **build_index(db, src, quiet=quiet)}


def lookup(vas: list[int], with_ida: bool = True) -> list[dict]:
    if not DB_PATH.exists():
        return [{"error": "Index not built. Run: python scripts/va_to_source.py --build"}]

    con = sqlite3.connect(DB_PATH)
    stale = check_fresh(con)
    if stale:
        con.close()
        return [dict(stale, va=hex(va)) for va in vas]
    # Probe IDA once per batch so a down plugin costs one timeout, not one per VA.
    ida_up = with_ida and (_ida_rpc("get_metadata", {}) is not None)
    results = []
    for va in vas:
        # Exact function match
        row = con.execute(
            "SELECT file, line, name FROM funcs WHERE va = ?", (va,)
        ).fetchone()
        if row:
            r = {"va": hex(va), "file": row[0], "line": row[1],
                 "name": row[2], "match": "function"}
        else:
            # Nearest function before this VA
            row = con.execute(
                "SELECT file, line, name, va FROM funcs WHERE va <= ? ORDER BY va DESC LIMIT 1",
                (va,)
            ).fetchone()
            if row:
                r = {"va": hex(va), "file": row[0], "line": row[1],
                     "name": row[2], "offset": va - row[3], "match": "inside_function"}
            else:
                r = {"va": hex(va), "error": "not found"}
        # Enrich with IDA's authoritative name (our ported ground truth) when it
        # has something better than a bare sub_XXXX.
        if ida_up and "error" not in r:
            nm = _ida_name_for(va)
            if nm and not nm.lower().startswith("sub_"):
                r["ida_name"] = nm
        results.append(r)

    con.close()
    return results


def va_to_decompile(va: int) -> dict:
    """One-call address hub: the recomp C we run (file:line) + IDA's Hex-Rays
    decompile of the enclosing function. Degrades to recomp-only if IDA is down."""
    base = lookup([va])[0]
    dec = _ida_rpc("decompile_function", {"address": hex(va)})
    if isinstance(dec, str):
        base["ida_decompile"] = dec
    elif dec is None:
        base["ida_decompile"] = None  # IDA not reachable / no function
    return base


def reverse_lookup(file: str, line: int) -> dict:
    if not DB_PATH.exists():
        return {"error": "Index not built"}
    con = sqlite3.connect(DB_PATH)
    stale = check_fresh(con)
    if stale:
        con.close()
        return stale
    row = con.execute(
        "SELECT va, name FROM funcs WHERE file LIKE ? AND line <= ? ORDER BY line DESC LIMIT 1",
        (f"%{file}%", line)
    ).fetchone()
    con.close()
    if row:
        return {"va": hex(row[0]), "name": row[1]}
    return {"error": "not found"}


# ── MCP server (stdio transport, no external deps) ───────────────────────────

MCP_TOOLS = [
    {
        "name": "va_to_source",
        "description": "Map one or more guest virtual addresses to ppc_recomp file:line:function.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "addresses": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Guest VAs as hex strings, e.g. [\"0x824DB158\"]"
                }
            },
            "required": ["addresses"]
        }
    },
    {
        "name": "source_to_va",
        "description": "Find the guest VA for a given ppc_recomp filename and line number.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string", "description": "Partial filename, e.g. 'ppc_recomp.61'"},
                "line": {"type": "integer"}
            },
            "required": ["file", "line"]
        }
    },
    {
        "name": "build_va_index",
        "description": "Rebuild the VA-to-source index from all ppc_recomp files. Run after XenonRecomp regenerates.",
        "inputSchema": {"type": "object", "properties": {}}
    },
    {
        "name": "va_to_decompile",
        "description": "IDA-backed address hub: map a guest VA to the recompiled C we run "
                       "(file:line) AND IDA's Hex-Rays decompile of the enclosing function. "
                       "Falls back to recomp-only if the IDA plugin isn't running.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": {"type": "string", "description": "Guest VA as hex, e.g. \"0x825FDA60\""}
            },
            "required": ["address"]
        }
    }
]


def mcp_server():
    import io
    stdin = io.TextIOWrapper(sys.stdin.buffer, encoding="utf-8")
    stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")

    def send(obj):
        line = json.dumps(obj)
        stdout.write(line + "\n")
        stdout.flush()

    def handle(req):
        method = req.get("method", "")
        rid = req.get("id")

        if method == "initialize":
            return {"jsonrpc": "2.0", "id": rid, "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "va-to-source", "version": "1.0"}
            }}

        if method == "tools/list":
            return {"jsonrpc": "2.0", "id": rid, "result": {"tools": MCP_TOOLS}}

        if method == "tools/call":
            name = req["params"]["name"]
            args = req["params"].get("arguments", {})

            if name == "va_to_source":
                vas = [int(a, 16) for a in args["addresses"]]
                result = lookup(vas)
                text = json.dumps(result, indent=2)
            elif name == "source_to_va":
                result = reverse_lookup(args["file"], args["line"])
                text = json.dumps(result, indent=2)
            elif name == "va_to_decompile":
                result = va_to_decompile(int(args["address"], 16))
                text = json.dumps(result, indent=2)
            elif name == "build_va_index":
                try:
                    build_index()
                    text = "Index built successfully."
                except Exception as e:
                    text = f"Error: {e}"
            else:
                text = f"Unknown tool: {name}"

            return {"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": text}]
            }}

        if method == "notifications/initialized":
            return None

        return {"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": f"Unknown method: {method}"}}

    for line in stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
            resp = handle(req)
            if resp is not None:
                send(resp)
        except Exception as e:
            send({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": str(e)}})


def selftest() -> bool:
    """HERMETIC freshness proof: builds a miniature REAL index in a temp source tree and
    database, then attacks it the way the director attacked the first attempt.

    The first version was rejected because it mutated the STORED DB fingerprint instead of
    a source file, so it never demonstrated the actual failure. This one edits source BYTES
    while preserving both size and mtime - precisely the case that defeated the old
    metadata-only digest - and requires BOTH lookup directions to refuse.
    """
    import shutil, tempfile
    global DB_PATH, RECOMP_DIR
    NL = chr(10)
    ok = True
    live_db, live_src = DB_PATH, RECOMP_DIR
    tmpdir = Path(tempfile.mkdtemp(prefix="vaidx_selftest_"))
    print("-- va_index hermetic freshness self-test --")
    try:
        src = tmpdir / "src"
        src.mkdir()
        db = tmpdir / "va_index.db"
        f1 = ("PPC_FUNC_IMPL(__imp__sub_82000010) {@"
              "\tPPC_FUNC_PROLOGUE();@"
              "loc_82000014:@"
              "\treturn;@"
              "}@").replace("@", NL)
        f2 = ("PPC_FUNC_IMPL(__imp__sub_82000020) {@"
              "\treturn;@"
              "}@").replace("@", NL)
        (src / "ppc_recomp.1.cpp").write_text(f1, encoding="utf-8")
        (src / "ppc_recomp.2.cpp").write_text(f2, encoding="utf-8")
        DB_PATH, RECOMP_DIR = db, src

        r = build_index(db, src, quiet=True)
        if r["funcs"] != 2:
            ok = False; print("   FAIL 1 build: %s" % r)
        else:
            print("   ok   1 miniature index built (2 funcs)")

        fw = lookup([0x82000010], with_ida=False)[0]
        rv = reverse_lookup("ppc_recomp.1", 1)
        if fw.get("name") == "sub_82000010" and rv.get("name") == "sub_82000010":
            print("   ok   2 exact forward and reverse mapping")
        else:
            ok = False; print("   FAIL 2 mapping: %s / %s" % (fw, rv))

        victim = src / "ppc_recomp.2.cpp"
        st = victim.stat()
        data = victim.read_bytes()
        edited = data.replace(b"sub_82000020", b"sub_82000029")
        if len(edited) != len(data):
            ok = False; print("   FAIL 3 test edit changed size")
        victim.write_bytes(edited)
        os.utime(victim, ns=(st.st_atime_ns, st.st_mtime_ns))
        st2 = victim.stat()
        if st2.st_size != st.st_size or st2.st_mtime_ns != st.st_mtime_ns:
            ok = False; print("   FAIL 3 could not preserve size+mtime for the attack")
        else:
            fw = lookup([0x82000010], with_ida=False)[0]
            rv = reverse_lookup("ppc_recomp.1", 1)
            if fw.get("stale") and rv.get("stale"):
                print("   ok   3 same-size edit + restored mtime -> BOTH directions REFUSE")
                print("        (the exact case the metadata-only digest ACCEPTED)")
            else:
                ok = False
                print("   FAIL 3 stale index still answered: %s / %s" % (fw, rv))

        build_index(db, src, quiet=True)
        fw = lookup([0x82000029], with_ida=False)[0]
        rv = reverse_lookup("ppc_recomp.2", 1)
        if fw.get("name") == "sub_82000029" and rv.get("name") == "sub_82000029":
            print("   ok   4 rebuild restores exact bidirectional mapping")
        else:
            ok = False; print("   FAIL 4 after rebuild: %s / %s" % (fw, rv))

        lock = db.with_name(db.name + ".buildlock")
        lock.write_text("held")
        failed = False
        try:
            build_index(db, src, quiet=True)
        except RuntimeError:
            failed = True
        finally:
            lock.unlink()
        fw = lookup([0x82000029], with_ida=False)[0]
        if failed and fw.get("name") == "sub_82000029":
            print("   ok   5 pre-existing lock refused; prior db usable")
        else:
            ok = False; print("   FAIL 5 failed=%s lookup=%s" % (failed, fw))

        con = sqlite3.connect(db)
        con.execute("DROP TABLE meta")
        con.commit(); con.close()
        fw = lookup([0x82000029], with_ida=False)[0]
        if fw.get("stale"):
            print("   ok   6 pre-repair db with no fingerprint -> refuses")
        else:
            ok = False; print("   FAIL 6 fingerprint-less db answered: %s" % fw)
    finally:
        DB_PATH, RECOMP_DIR = live_db, live_src
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("-- va_index hermetic self-test: %s --" % ("PASS" if ok else "FAIL"))
    return ok


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", action="store_true", help="Build/rebuild the VA index")
    parser.add_argument("--ensure-index", action="store_true", help="Rebuild only when the source content fingerprint differs")
    parser.add_argument("--mcp", action="store_true", help="Run as MCP stdio server")
    parser.add_argument("--selftest", action="store_true",
                        help="Prove the freshness guard fails closed")
    parser.add_argument("addresses", nargs="*", help="Guest VAs to look up (hex)")
    args = parser.parse_args()

    if args.selftest:
        sys.exit(0 if selftest() else 1)
    elif args.build:
        build_index()
    elif args.ensure_index:
        print(json.dumps(ensure_fresh_index(), indent=2))
    elif args.mcp:
        mcp_server()
    elif args.addresses:
        vas = [int(a, 16) for a in args.addresses]
        for r in lookup(vas):
            print(json.dumps(r, indent=2))
    else:
        parser.print_help()
