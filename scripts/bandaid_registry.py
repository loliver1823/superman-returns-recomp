#!/usr/bin/env python3
"""
bandaid_registry.py — docs/TOOLING_PLAN.md L7 (bandaid governance).

Keeps docs/BANDAIDS.md honest against the actual code. The rule at the top of
BANDAIDS.md is "no new bandaid without removing one"; today that's enforced by
memory and rots. This tool cross-checks the table against the tree so the
governance is mechanical and CI-able.

What it does
------------
1. Parses the ACTIVE (non struck-through) rows of the BANDAIDS.md table into
   {bandaid, where, proper_fix}. Rows whose Bandaid OR Where cell is fully
   ~~struck through~~ are treated as resolved/removed and skipped.
2. Greps the code dirs (kernel/, app/, gfx/, guest/) for two things:
     - existing bandaid-ish markers: BANDAID, "FIX:", "guard", "cap", "bypass",
       "workaround", "skip", "fallback", "*Added <date>*" comments, etc.
     - the lightweight tagging convention this tool proposes (see below).
3. Cross-checks both directions and reports:
     (a) STALE  — table rows whose `where` file is gone, or for which no
                  plausible matching code token (function/VA/filename) was found.
     (b) ORPHAN — in-code BANDAID(...) tags whose slug is not represented by any
                  active table row.
4. Prints a report and exits non-zero when orphans (untracked BANDAID(...) tags)
   are found, so it can run as a pre-commit / CI gate. Stale rows are reported
   but, by default, do NOT fail the build (they need human review, not a block);
   pass --strict to also fail on stale rows. --quiet prints only the summary
   line and the failing items.

Tagging convention (proposed, adopt incrementally)
---------------------------------------------------
When you add or touch a bandaid in code, tag the site with a single-line comment:

    // BANDAID(<slug>): <one-line what + why>

  - <slug> is a short, stable, kebab/identifier-ish handle, e.g.
        dogma-guard-bypass, vdswap-fence-write, sub_82762640-uninit-guard.
    Prefer the primary `sub_XXXX` symbol or a terse phrase from the table row.
  - Put the SAME slug in a new "Slug" reference inside the BANDAIDS.md row's
    description (just write `BANDAID(<slug>)` somewhere in the Bandaid or
    Proper-fix cell) so this tool can pair the code tag to the row with zero
    guessing. Until a row carries a slug, pairing falls back to fuzzy token
    matching (function names / VAs / filenames) and the row is reported as
    "matched (fuzzy)" rather than "matched (tagged)".

This is intentionally cheap: one comment line, one token in the table. Adopt it
on new bandaids and opportunistically on old ones; the fuzzy matcher covers the
untagged backlog so nothing has to be migrated up-front.

Usage
-----
    python scripts/bandaid_registry.py            # full report, exit!=0 on orphans
    python scripts/bandaid_registry.py --quiet     # summary + failing items only
    python scripts/bandaid_registry.py --strict     # also fail on stale rows
    python scripts/bandaid_registry.py --json        # machine-readable dump

stdlib only.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BANDAIDS_MD = ROOT / "docs" / "BANDAIDS.md"
CODE_DIRS = ["kernel", "app", "gfx", "guest"]
CODE_EXTS = {".cpp", ".cc", ".cxx", ".c", ".hpp", ".hh", ".h", ".inl"}

# The convention tag: // BANDAID(<slug>): <one-line>
TAG_RE = re.compile(r"BANDAID\(\s*([A-Za-z0-9_\-./]+)\s*\)\s*:?(.*)")

# Bandaid-ish markers we surface in code even when untagged. Word-ish, case
# insensitive; tuned to the vocabulary actually used in this tree's comments.
MARKER_RE = re.compile(
    r"//.*\b("
    r"BANDAID|FIX:|HACK|XXX|TODO|workaround|work-around|bypass|"
    r"guard|cap(?:ped|s)?|clamp|skip|fallback|no-?op|stub|"
    r"early[- ]?(?:exit|return)|short-circuit|saturat\w*"
    r")\b",
    re.I,
)
# "*Added 2026-02-15.*" / "*Updated ...*" dated-comment signature.
DATED_RE = re.compile(r"\*(?:Added|Updated)\s+\d{4}-\d{2}-\d{2}", re.I)

# Tokens we can fuzzily pull from a table row's `where`/description and look for
# in code: guest function symbols and guest VAs.
SUBFUNC_RE = re.compile(r"sub_[0-9A-Fa-f]{6,8}")
VA_RE = re.compile(r"0x[0-9A-Fa-f]{6,8}")
# Filenames named in the `where` column, e.g. ppc_recomp.106.cpp, kernel/stubs.cpp.
FILE_RE = re.compile(r"[A-Za-z0-9_./\\]+\.(?:cpp|cc|cxx|c|hpp|hh|h|inl)")
# Host / kernel-stub C++ identifiers a row may name instead of a sub_ symbol:
# Xbox kernel imports (Xam*/Nt*/Ke*/Vd*/Xex*/Rtl*/Ex*) and CamelCase host fns
# (pumpRenderState, setupFakeGpuDevice, oorGetOrCreateCS, ...). Min length keeps
# out short English words. Matched against an identifier index built from code.
IDENT_RE = re.compile(
    r"\b((?:Xam|Nt|Ke|Vd|Xex|Rtl|Ex|Mm|Ob|Io|Ps|Dbg|XMA)[A-Z][A-Za-z0-9_]{2,}"
    r"|[a-z][a-zA-Z0-9_]*[A-Z][A-Za-z0-9_]{3,})\b")


def strip_md(s):
    """Collapse markdown emphasis/inline-code so token matching is clean."""
    s = s.replace("`", "")
    return s.strip()


def is_struck(cell):
    """True if the cell is fully struck through (~~...~~), i.e. resolved."""
    c = cell.strip()
    return c.startswith("~~") and c.endswith("~~") and len(c) > 4


def parse_table(md_text):
    """Yield active (non-resolved) bandaid rows as dicts.

    The table has 3 logical columns: Bandaid | Where | Proper fix. Note the file
    uses leading `||` / `|||` on some rows (cosmetic); we normalize by splitting
    on '|' and dropping empty edge cells.
    """
    rows = []
    for raw in md_text.splitlines():
        line = raw.strip()
        if not line.startswith("|"):
            continue
        # Split and drop the empty strings produced by leading/trailing pipes.
        cells = [c.strip() for c in line.split("|")]
        cells = [c for c in cells if c != ""]
        if len(cells) < 3:
            continue
        bandaid, where, proper = cells[0], cells[1], "|".join(cells[2:])
        # Skip the header and separator rows.
        if bandaid.lower() in ("bandaid",):
            continue
        if set(bandaid) <= set("-: "):
            continue
        # A row is "resolved/removed" when the Bandaid name OR the Where cell is
        # fully struck-through. Those are history, not active workarounds.
        if is_struck(bandaid) or is_struck(where):
            rows.append({"_active": False})
            continue
        rows.append({
            "_active": True,
            "bandaid": strip_md(bandaid),
            "where": strip_md(where),
            "proper_fix": strip_md(proper),
        })
    return rows


def row_tokens(row):
    """Pull fuzzy-match tokens from a row: functions, VAs, filenames, slug tags."""
    blob = " ".join((row["bandaid"], row["where"], row["proper_fix"]))
    funcs = set(m.lower() for m in SUBFUNC_RE.findall(blob))
    vas = set(m.lower() for m in VA_RE.findall(blob))
    # VAs and sub_ overlap conceptually but are matched the same way; keep both.
    files = set()
    for f in FILE_RE.findall(row["where"]) + FILE_RE.findall(blob):
        files.add(Path(f.replace("\\", "/")).name.lower())
    slugs = set(m.group(1).lower() for m in TAG_RE.finditer(blob))
    idents = set(IDENT_RE.findall(blob))
    return {"funcs": funcs, "vas": vas, "files": files, "slugs": slugs,
            "idents": idents}


def iter_code_files():
    for d in CODE_DIRS:
        base = ROOT / d
        if not base.is_dir():
            continue
        for p in base.rglob("*"):
            if p.is_file() and p.suffix.lower() in CODE_EXTS:
                yield p


def scan_code():
    """Scan code dirs once. Returns (tags, markers, index).

    tags    : list of {slug, file, line, text}             — BANDAID(...) tags
    markers : list of {file, line, text}                   — bandaid-ish comments
    index   : dict mapping token -> set(relpath) for fuzzy row matching:
              funcs (sub_XXXX), vas (0x...), and filenames present in the tree.
    """
    tags = []
    markers = []
    func_idx = {}
    va_idx = {}
    ident_set = set()
    file_set = set()

    for p in iter_code_files():
        rel = p.relative_to(ROOT).as_posix()
        file_set.add(p.name.lower())
        try:
            text = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for i, line in enumerate(text.splitlines(), 1):
            for m in SUBFUNC_RE.findall(line):
                func_idx.setdefault(m.lower(), set()).add(rel)
            for m in VA_RE.findall(line):
                va_idx.setdefault(m.lower(), set()).add(rel)
            for m in IDENT_RE.findall(line):
                ident_set.add(m)

            tm = TAG_RE.search(line)
            if tm:
                tags.append({
                    "slug": tm.group(1).lower(),
                    "file": rel,
                    "line": i,
                    "text": line.strip()[:160],
                })
                # A BANDAID(...) tag is itself a marker; don't double-list below.
                continue
            if MARKER_RE.search(line) or DATED_RE.search(line):
                markers.append({"file": rel, "line": i, "text": line.strip()[:160]})

    return tags, markers, {
        "funcs": func_idx,
        "vas": va_idx,
        "idents": ident_set,
        "files": file_set,
    }


def match_row(row, index):
    """Decide how an active row maps to code. Returns (status, evidence).

    status in {"tagged", "fuzzy", "file-only", "stale-file", "stale-nomatch"}.
    """
    toks = row_tokens(row)

    # 1) Explicit slug tag present in the table — strongest signal. (Pairing to
    #    the actual code tag is validated separately in the orphan pass.)
    if toks["slugs"]:
        return "tagged", {"slugs": sorted(toks["slugs"])}

    # 2) Does the named file still exist anywhere in the tree?
    named_files = toks["files"]
    file_exists = None
    if named_files:
        file_exists = any(f in index["files"] for f in named_files)

    # 3) Symbol evidence in code: guest sub_ funcs, VAs, and host/kernel idents.
    hit_funcs = sorted(f for f in toks["funcs"] if f in index["funcs"])
    hit_vas = sorted(v for v in toks["vas"] if v in index["vas"])
    hit_idents = sorted(i for i in toks["idents"] if i in index["idents"])

    if hit_funcs or hit_vas or hit_idents:
        ev = {}
        if hit_funcs:
            ev["funcs"] = hit_funcs[:6]
        if hit_vas:
            ev["vas"] = hit_vas[:6]
        if hit_idents:
            ev["idents"] = hit_idents[:6]
        if named_files:
            ev["file_exists"] = bool(file_exists)
        return "fuzzy", ev

    # No symbol evidence. If a concrete file was named and it's gone -> stale.
    if named_files and file_exists is False:
        return "stale-file", {"missing_files": sorted(named_files)}

    # No symbol matched but the named file exists. Weak-but-present: the bandaid
    # lives in a file we can see, we just couldn't pin a token. Not "stale".
    if named_files and file_exists:
        return "file-only", {"files": sorted(named_files)}

    # Nothing to go on (vague/absent file, no tokens). Flag for human review.
    return "stale-nomatch", {
        "named_files": sorted(named_files),
        "tokens_sought": sorted(toks["funcs"] | toks["vas"] | toks["idents"]),
    }


def find_orphans(tags, active_slugs):
    """Code BANDAID(slug) tags whose slug has no active table row."""
    return [t for t in tags if t["slug"] not in active_slugs]


def main():
    ap = argparse.ArgumentParser(
        description="Cross-check docs/BANDAIDS.md against in-code bandaid markers/tags (L7).")
    ap.add_argument("--quiet", action="store_true",
                    help="Print only the summary line and failing items.")
    ap.add_argument("--strict", action="store_true",
                    help="Also exit non-zero when stale rows are found (not just orphans).")
    ap.add_argument("--json", action="store_true",
                    help="Emit a machine-readable JSON dump instead of the text report.")
    args = ap.parse_args()

    # Be friendly to the Windows console (cp1252) — never die on a stray char.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

    if not BANDAIDS_MD.is_file():
        print(f"[bandaid] ERROR: {BANDAIDS_MD} not found", file=sys.stderr)
        return 2

    rows = parse_table(BANDAIDS_MD.read_text(encoding="utf-8", errors="replace"))
    active = [r for r in rows if r.get("_active")]
    resolved_n = sum(1 for r in rows if not r.get("_active"))

    tags, markers, index = scan_code()

    matched_tagged, matched_fuzzy, matched_file, stale = [], [], [], []
    active_slugs = set()
    for row in active:
        status, ev = match_row(row, index)
        for s in row_tokens(row)["slugs"]:
            active_slugs.add(s)
        rec = {"bandaid": row["bandaid"], "where": row["where"],
               "status": status, "evidence": ev}
        if status == "tagged":
            matched_tagged.append(rec)
        elif status == "fuzzy":
            matched_fuzzy.append(rec)
        elif status == "file-only":
            matched_file.append(rec)
        else:
            stale.append(rec)

    orphans = find_orphans(tags, active_slugs)

    summary = {
        "active_rows": len(active),
        "resolved_rows": resolved_n,
        "matched_tagged": len(matched_tagged),
        "matched_fuzzy": len(matched_fuzzy),
        "matched_file_only": len(matched_file),
        "stale": len(stale),
        "code_tags": len(tags),
        "orphan_tags": len(orphans),
        "code_markers": len(markers),
    }

    if args.json:
        print(json.dumps({
            "summary": summary,
            "matched_tagged": matched_tagged,
            "matched_fuzzy": matched_fuzzy,
            "matched_file_only": matched_file,
            "stale": stale,
            "orphans": orphans,
        }, indent=2))
    else:
        report(summary, matched_tagged, matched_fuzzy, matched_file,
               stale, orphans, markers, args.quiet)

    # Exit policy: orphan tags always fail (untracked bandaid in code). Stale
    # rows fail only under --strict (they need human review, not a hard block).
    fail = bool(orphans) or (args.strict and bool(stale))
    return 1 if fail else 0


def report(summary, tagged, fuzzy, file_only, stale, orphans, markers, quiet):
    def hdr(t):
        print(f"\n=== {t} ===")

    if not quiet:
        print("[bandaid] L7 registry cross-check")
        print("  active rows       : {active_rows}".format(**summary))
        print("  resolved rows     : {resolved_rows} (struck-through, skipped)".format(**summary))
        print("  matched (tagged)  : {matched_tagged}".format(**summary))
        print("  matched (fuzzy)   : {matched_fuzzy}".format(**summary))
        print("  matched (file-only): {matched_file_only}  (file present, no token pinned)".format(**summary))
        print("  STALE rows        : {stale}  (file gone / nothing to match)".format(**summary))
        print("  code BANDAID tags : {code_tags}".format(**summary))
        print("  ORPHAN tags       : {orphan_tags}  (tag in code, no table row)".format(**summary))
        print("  bandaid markers   : {code_markers}  (untagged candidate sites)".format(**summary))

    if orphans:
        hdr("ORPHAN tags (in code, not in BANDAIDS.md) -- FAIL")
        for t in orphans:
            print(f"  BANDAID({t['slug']})  {t['file']}:{t['line']}")
            print(f"      {t['text']}")

    if stale:
        hdr("STALE rows (no plausible matching code) -- review")
        for r in stale:
            print(f"  [{r['status']}] {r['bandaid']}  (where: {r['where']})")
            ev = r["evidence"]
            if ev.get("missing_files"):
                print(f"      missing file(s): {', '.join(ev['missing_files'])}")
            elif ev.get("tokens_sought"):
                ts = ev["tokens_sought"]
                print(f"      sought {len(ts)} token(s), none found in code"
                      + (f": {', '.join(ts[:6])}" if ts else " (no symbol/VA tokens in row)"))

    if quiet:
        verdict = "FAIL" if (orphans or stale) else "OK"
        print(f"\n[bandaid] {verdict}: {summary['orphan_tags']} orphan, {summary['stale']} stale.")
        return

    hdr("Matched (tagged) -- paired by BANDAID(slug)")
    for r in tagged:
        print(f"  OK   {r['bandaid']}  ->  {', '.join(r['evidence'].get('slugs', []))}")
    hdr("Matched (fuzzy) -- paired by function/VA/ident token")
    for r in fuzzy:
        ev = r["evidence"]
        bits = []
        if ev.get("funcs"):
            bits.append("fn=" + ",".join(ev["funcs"]))
        if ev.get("vas"):
            bits.append("va=" + ",".join(ev["vas"]))
        if ev.get("idents"):
            bits.append("id=" + ",".join(ev["idents"]))
        print(f"  ~    {r['bandaid']}  [{'; '.join(bits)}]")

    hdr("Matched (file-only) -- named file exists, no token pinned")
    for r in file_only:
        print(f"  .    {r['bandaid']}  ({', '.join(r['evidence'].get('files', []))})")

    print(f"\n[bandaid] {len(markers)} untagged bandaid-ish marker(s) in code "
          "(consider adopting the BANDAID(slug) convention). Run with --json to dump them.")


if __name__ == "__main__":
    sys.exit(main())
