#!/usr/bin/env python3
"""package_port.py - build the PORT FEATURE packages (docs/MODS.md section 7) as clean patches.

A port feature ships as SRPATCH1 deltas against the player's own disc: our bytes plus copy
instructions, never the game's data. The exe applies them at boot into mods/.cache/gen/ and
serves each result as a loose asset under DATA/_workinprogress/<feature>/ (the engine's own
per-asset override storage, priority 0x7D0, keyed by the file stem). For each feature this
tool

  1. runs the feature's builder (the tracked recipes, e.g. tools/apt/mods/custom_frontend/)
     on the disc, in memory;
  2. finds every top-level entry of each output container that differs from the disc's
     (e.g. the 5 Apt movies the custom front end edits inside ui.AST);
  3. encodes each one with srpatch.encode (base = the disc entry; COPYF may copy from
     anywhere in the base FILE) and refuses it unless it is CLEAN: no ADD run holds a 32-byte
     window that occurs anywhere in the base file;
  4. applies the patches exactly as the exe does (srmods.generate_assets) and requires every
     generated asset to equal the builder's entry BYTE FOR BYTE;
  5. writes data/port_features/<id>/ (feature.json + <ENTRY>.srpatch).

  python tools/srmods/package_port.py               # build + verify + write every feature
  python tools/srmods/package_port.py --check       # the same, write nothing; fails if the
                                                    # committed package is stale
  python tools/srmods/package_port.py --feature custom_frontend
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import struct
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools", "apt"))
import srpatch  # noqa: E402
import srmods  # noqa: E402

DISC = os.path.join(REPO, "Superman Returns (USA)")
OUT_ROOT = os.path.join(REPO, "data", "port_features")


def _custom_frontend(disc):
    import build_custom_frontend as bcf
    new_ui, _ = bcf.build(ui_path=os.path.join(disc, "DATA", "ui.AST"), write=False, log=lambda *a: None)
    return {"DATA/ui.AST": new_ui}


def _pause_map(disc):
    # iter-466 map lane: the useful pause map (docs/MAP.md): SCRMAP's marker renderer
    import build_custom_frontend as bcf
    new_ui, _ = bcf.build(ui_path=os.path.join(disc, "DATA", "ui.AST"),
                          patch_dir=os.path.join(REPO, "tools", "apt", "mods", "pause_map"),
                          write=False, log=lambda *a: None, mod="pause_map")
    return {"DATA/ui.AST": new_ui}


def _autosave(disc):
    import build_custom_frontend as bcf
    with open(os.path.join(disc, "DATA", "baseworl.AST"), "rb") as source:
        base = source.read()
    return {"DATA/baseworl.AST": bcf.autosave_lua(base)}


FEATURES = {
    "custom_frontend": {
        "name": "Custom front end",
        "description": "MAIN MENU (NEW GAME / LOAD GAME / SETTINGS / EXIT) in the game's own popup chrome, "
                       "universal settings, MODS and Dev Tools pages, and the pause menu "
                       "(docs/CUSTOM_FRONTEND.md).",
        "option": None,
        "recipe": "tools/apt/mods/custom_frontend",
        "ext": "ast",       # Apt movie packages are loose type 'ast'
        "build": _custom_frontend,
    },
    "pause_map": {
        "name": "Useful pause map",
        "description": "The pause map marks the current objective and the next story mission, the races and "
                       "Bizarro statues (unlocked, completed, best time / score) and the kittens found, from "
                       "the game's own state; mods add markers or maps (docs/MAP.md).",
        "option": None,
        "recipe": "tools/apt/mods/pause_map",
        "ext": "ast",
        "build": _pause_map,
        # tracked files shipped as they are (our own data, no game bytes): the map definition
        "extra": {"map.json": "tools/apt/mods/pause_map/map.json"},
    },
    "autosave_without_asking": {
        "name": "Autosave without asking",
        "description": "The game's chapter-advance save request without its confirmation prompt.",
        "option": "autosave_without_asking",
        "recipe": "tools/apt/mods/custom_frontend/srautosave.aptasm",
        "ext": "ast",  # the changed top-level flow container, served as a loose asset
        "build": _autosave,
    },
}


def top_entries(buf):
    """-> [(name, id, offset, size)] of a BGFA container in memory."""
    count, toc_off = struct.unpack_from("<II", buf, 0x0C)
    wA, wId, wOff, wSize, wUnc, shift = buf[0x21:0x27]
    ntags, namelen = struct.unpack_from("<II", buf, 0x28)
    rec = 1 + wA + wId + wOff + wSize + wUnc + namelen
    p = toc_off + 4 * ntags
    out = []
    for i in range(count):
        r = buf[p + rec * i: p + rec * (i + 1)]
        q = 1 + wA
        ident = int.from_bytes(r[q:q + wId], "little"); q += wId
        off = int.from_bytes(r[q:q + wOff], "little") << shift; q += wOff
        size = int.from_bytes(r[q:q + wSize], "little"); q += wSize + wUnc
        name = r[q:q + namelen].split(b"\0")[0].decode("latin1") if namelen else ""
        out.append((name, ident, off, size))
    return out


def package(fid, spec, disc):
    t0 = time.time()
    outputs = spec["build"](disc)
    files, assets, report, base_pins, want = {}, [], [], {}, {}
    for rel, new in sorted(outputs.items()):
        base_path = os.path.join(disc, rel)
        base_file = open(base_path, "rb").read()
        base_pins[rel] = {"size": len(base_file), "toc_sha256": srmods.toc_sha256(base_path)}
        old = {(n.upper() or "#%016x" % i): (o, s) for n, i, o, s in top_entries(base_file)}
        for n, i, o, s in top_entries(new):
            key = n.upper() or "#%016x" % i
            data = new[o:o + s]
            if key not in old:
                raise SystemExit("%s: %s adds the entry %s; a port feature can only patch existing entries" % (fid, rel, key))
            bo, bs = old[key]
            base_entry = base_file[bo:bo + bs]
            if data == base_entry:
                continue
            patch, st = srpatch.encode(base_entry, data, file=base_file)
            runs = srpatch.add_runs(srpatch.parse(patch)[1])
            dirty = srpatch.dirty_windows(base_file, runs)
            if dirty:
                raise SystemExit("%s: %s:%s is NOT CLEAN: %d ADD window(s) of %d bytes occur in the base file"
                                 % (fid, rel, key, len(dirty), srpatch.WINDOW))
            pname = "%s.srpatch" % key.replace("#", "_")
            files[pname] = patch
            asset = "DATA/_workinprogress/%s/%s.%s" % (fid, key, spec["ext"])
            assets.append({"asset": asset, "base": "%s:%s" % (rel, key), "patch": pname})
            want[asset] = data
            report.append("%s:%s %d -> %d B: patch %d B (ADD %d B in %d runs, max %d; COPY %d B; COPYF %d B) -> %s"
                          % (rel, key, bs, len(data), len(patch), st["add_bytes"], st["add_runs"], st["max_add_run"],
                             st["copy_bytes"], st["copyf_bytes"], asset))
    fj = {"format": 1, "id": fid, "name": spec["name"], "description": spec["description"],
          "option": spec["option"], "recipe": spec["recipe"], "base": base_pins, "assets": assets,
          "assets_sha256": {k: hashlib.sha256(v).hexdigest() for k, v in sorted(want.items())}}
    files["feature.json"] = (json.dumps(fj, indent=2) + "\n").encode("utf-8")
    for name, src in sorted(spec.get("extra", {}).items()):
        files[name] = open(os.path.join(REPO, src), "rb").read()
        report.append("%s <- %s (%d B, tracked as is)" % (name, src, len(files[name])))
    # verify exactly as the exe applies it
    tmp = tempfile.mkdtemp(prefix="srport_")
    try:
        for k, v in files.items():
            open(os.path.join(tmp, k), "wb").write(v)
        gen = srmods.generate_assets("verify", srmods._gen_list(fj, "assets", tmp), disc, os.path.join(tmp, "cache"))
        for asset, data in want.items():
            got = open(os.path.join(gen, asset), "rb").read()
            if got != data:
                raise SystemExit("%s: the generated %s differs from the builder's entry" % (fid, asset))
        report.append("verify: %d generated asset(s) == the builder's entries, byte for byte" % len(want))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    report.append("%.1f s" % (time.time() - t0))
    return files, report


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="Build the port-feature patch packages.")
    ap.add_argument("--feature", action="append", help="feature id (default: all)")
    ap.add_argument("--check", action="store_true", help="verify only; fail if the committed package is stale")
    ap.add_argument("--disc", default=DISC)
    ap.add_argument("--out", default=OUT_ROOT)
    a = ap.parse_args(argv)
    rc = 0
    for fid in a.feature or sorted(FEATURES):
        files, report = package(fid, FEATURES[fid], a.disc)
        print("== %s" % fid)
        for line in report:
            print("   " + line)
        dst = os.path.join(a.out, fid)
        if a.check:
            stale = [k for k, v in files.items()
                     if not os.path.isfile(os.path.join(dst, k)) or open(os.path.join(dst, k), "rb").read() != v]
            if os.path.isdir(dst):
                stale += [f + " (not produced any more)" for f in os.listdir(dst) if f not in files]
            if stale:
                print("   STALE: %s" % ", ".join(sorted(stale)))
                rc = 1
            else:
                print("   the committed package is current")
            continue
        if os.path.isdir(dst):
            shutil.rmtree(dst)
        os.makedirs(dst)
        for k, v in sorted(files.items()):
            open(os.path.join(dst, k), "wb").write(v)
        print("   wrote %s (%d files, %d B)" % (dst, len(files), sum(len(v) for v in files.values())))
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
