#!/usr/bin/env python3
"""release_lint.py - a player build must never carry game bytes (owner rule: "never ship game bytes").

    python tools/player/release_lint.py <staged player dir> [--disc "<...\\Superman Returns (USA)>"] [--no-blocks]

Checks every staged file (the disc junction itself is skipped - it points at the player's own copy and is never
copied; a REAL directory or file copy of the disc fails):
  1. FAIL  byte-identical to a disc file (size index first, then sha256).
  2. FAIL  shares disc content: any 4 KiB-aligned block of a staged file >= 64 KiB that equals a 4 KiB-aligned block
           of a disc file (non-uniform blocks only; catches extracted or embedded verbatim assets). --no-blocks skips it.
  3. FAIL  a staged path under a data/credits.json game_derived class verbatim / capture / screenshot;
     WARN  translated / metadata (shipping them is a recorded decision, so the lint lists them, it does not block);
     ok    clean-patch (our bytes + copy instructions against the player's own disc).
  4. FAIL  secret-looking content (private keys, cloud / GitHub / API tokens) in text-like files.
Exit 0 = PASS (warnings allowed), 1 = FAIL. Writes <dir>/release_lint.json.

Idea from the universal-modder `um publish check` comparison (2026-10-09); our own code.
"""
import argparse
import fnmatch
import hashlib
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_DISC = os.path.join(REPO, "Superman Returns (USA)")
DISC_NAME = "Superman Returns (USA)"
BLOCK = 4096
FAIL_CLASSES = {"verbatim", "capture", "screenshot"}
WARN_CLASSES = {"translated", "metadata"}
SECRETS = [
    ("private key", re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----")),
    ("AWS access key", re.compile(rb"\bAKIA[0-9A-Z]{16}\b")),
    ("GitHub token", re.compile(rb"\b(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{36}\b|\bgithub_pat_[A-Za-z0-9_]{60,}\b")),
    ("Anthropic / OpenAI key", re.compile(rb"\bsk-(?:ant-)?[A-Za-z0-9_\-]{32,}\b")),
    ("fal / generic API key assignment", re.compile(rb"(?i)\b(?:fal_key|api[_-]?key|secret[_-]?key)\s*[:=]\s*['\"]?[A-Za-z0-9_\-]{24,}")),
]
TEXT_EXT = {".txt", ".md", ".json", ".ini", ".cfg", ".toml", ".yaml", ".yml", ".cmd", ".bat", ".ps1", ".py", ".lua", ".xml"}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def is_link(path):
    try:
        return os.path.islink(path) or (hasattr(os.path, "isjunction") and os.path.isjunction(path)) or \
            bool(os.lstat(path).st_file_attributes & 0x400)  # FILE_ATTRIBUTE_REPARSE_POINT
    except (OSError, AttributeError):
        return False


def staged_files(root):
    out, notes = [], []
    for dp, dns, fns in os.walk(root):
        keep = []
        for d in dns:
            full = os.path.join(dp, d)
            if is_link(full):
                notes.append("skipped link %s" % os.path.relpath(full, root))
                continue
            keep.append(d)
        dns[:] = keep
        for fn in fns:
            out.append(os.path.join(dp, fn))
    return out, notes


def uniform(b):
    return b.count(b[:1]) == len(b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("staged")
    ap.add_argument("--disc", default=DEFAULT_DISC)
    ap.add_argument("--no-blocks", action="store_true")
    a = ap.parse_args()
    root = os.path.abspath(a.staged)
    fails, warns = [], []
    files, notes = staged_files(root)
    rels = {f: os.path.relpath(f, root).replace("\\", "/") for f in files}

    # a real (non-link) disc directory inside the staged tree is a copy
    for f, r in rels.items():
        if r.split("/")[0] == DISC_NAME:
            fails.append(("disc copy", r))

    # 1 + 2: against the disc
    disc_files = []
    for dp, _dns, fns in os.walk(a.disc):
        disc_files += [os.path.join(dp, fn) for fn in fns]
    by_size = {}
    for d in disc_files:
        by_size.setdefault(os.path.getsize(d), []).append(d)
    disc_hash = {}
    for f, r in rels.items():
        sz = os.path.getsize(f)
        if sz and sz in by_size:
            h = sha256(f)
            for d in by_size[sz]:
                if d not in disc_hash:
                    disc_hash[d] = sha256(d)
                if disc_hash[d] == h:
                    fails.append(("identical to disc file " + os.path.relpath(d, a.disc).replace("\\", "/"), r))
    if not a.no_blocks:
        blocks = set()
        for d in disc_files:
            with open(d, "rb") as fh:
                while True:
                    b = fh.read(BLOCK)
                    if len(b) < BLOCK:
                        break
                    if not uniform(b):
                        blocks.add(hashlib.blake2b(b, digest_size=16).digest())
        for f, r in rels.items():
            if os.path.getsize(f) < 64 * 1024:
                continue
            hits = 0
            with open(f, "rb") as fh:
                while True:
                    b = fh.read(BLOCK)
                    if len(b) < BLOCK:
                        break
                    if not uniform(b) and hashlib.blake2b(b, digest_size=16).digest() in blocks:
                        hits += 1
            if hits:
                fails.append(("shares %d aligned 4 KiB block(s) with the disc" % hits, r))

    # 3: game_derived classes from data/credits.json
    try:
        gd = json.load(open(os.path.join(REPO, "data", "credits.json"), encoding="utf-8")).get("game_derived", [])
    except (OSError, ValueError):
        gd = []
        warns.append(("data/credits.json unreadable - game_derived classes not checked", ""))
    for f, r in rels.items():
        for g in gd:
            for pat in g.get("paths", []):
                p = pat.replace("**", "*")
                if fnmatch.fnmatch(r, p) or fnmatch.fnmatch(r, "*/" + p):
                    cls = g.get("class", "")
                    if cls in FAIL_CLASSES:
                        fails.append(("game_derived %s (%s)" % (cls, g.get("what", "")[:60]), r))
                    elif cls in WARN_CLASSES:
                        warns.append(("game_derived %s" % cls, r))
                    break

    # 4: secrets
    for f, r in rels.items():
        if os.path.splitext(f)[1].lower() not in TEXT_EXT or os.path.getsize(f) > 8 << 20:
            continue
        data = open(f, "rb").read()
        for name, rx in SECRETS:
            if rx.search(data):
                fails.append(("secret-looking content: " + name, r))

    # warnings collapse per class + directory
    wsum = {}
    for why, r in warns:
        key = (why, r.rsplit("/", 1)[0] if "/" in r else r)
        wsum[key] = wsum.get(key, 0) + 1
    report = {"staged": root, "disc": a.disc, "files": len(files), "notes": notes,
              "fails": [{"why": w, "path": p} for w, p in fails],
              "warnings": [{"why": w, "dir": d, "count": n} for (w, d), n in sorted(wsum.items())],
              "verdict": "FAIL" if fails else "PASS"}
    json.dump(report, open(os.path.join(root, "release_lint.json"), "w", encoding="utf-8"), indent=1)
    print("release_lint: %d staged files, %d disc files%s" % (len(files), len(disc_files),
                                                              "" if a.no_blocks else ", block scan on"))
    for n in notes:
        print("  note: " + n)
    for (w, d), n in sorted(wsum.items()):
        print("  WARN  %-28s %4d file(s) under %s" % (w, n, d))
    for w, p in fails:
        print("  FAIL  %s: %s" % (w, p))
    print("RELEASE_LINT %s" % report["verdict"])
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
