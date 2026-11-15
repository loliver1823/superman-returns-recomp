#!/usr/bin/env python3
"""srmods.py - the mod framework's tooling (docs/MODS.md). It is also the REFERENCE for
kernel/mod_manager.cpp: the same boot decision, manifest rules, load order, generated
assets and conflict rules (tools/srmods/rt_srmods.py checks the two against each other).

  list         the mods (load order, status), the port features, the conflicts
  lint MOD     check one mod folder or .zip (manifest, paths, patches, loose assets)
  order [--set a,b,c] [--enable X] [--disable X]
               show / change the player's load order in mods/mods.json (top wins)
  conflicts    every asset stem / file path two enabled layers both provide
  simulate     what the game registers from DATA\\_workinprogress (current host listing vs
               the Xenia-faithful one, SR_NTQDIR_XENIA): the scan passes, the stem parser,
               the 29 registered extensions
  gen --out D  generate the patched assets (port features + mods' "patches") like the exe
  pack MOD --out F.zip     zip a mod folder
  fingerprint  the data fingerprint for mod.json "game"

Options: --root R (content root: holds 'Superman Returns (USA)/' and 'mods/'; default the repo
root), --mods a;b / 0 / @player (like SR_MODS), --port 0|1 (like SR_PORT_FEATURES),
--automated (like SR_INSTANCE_ID). Nothing here writes the disc; only `order` writes
mods/mods.json. Stdlib only.
"""
from __future__ import annotations

import hashlib
import re
import json
import os
import struct
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import srpatch  # noqa: E402
import srvfs  # noqa: E402

DISC_DIR = "Superman Returns (USA)"
GAME_TAG = "SRX360-USA"
USA_RETAIL_FINGERPRINT = "fe43807d8d15db16"   # kernel/mod_manager.cpp kUsaRetailFingerprint
STATUSES = ["ok", "disabled", "bad-manifest", "bad-zip", "wrong-game", "missing-dependency",
            "declared-conflict", "bad-patch", "bad-content", "duplicate", "reserved", "off"]
# The 29 extensions the DATA\_workinprogress loose storage registers (table 0x82911D00).
LOOSE_EXTS = ["dds", "wmv", "fxh", "fx", "rss", "rml", "spk", "sns", "sgp", "sbm", "bmp", "pus", "map",
              "loc", "png", "lef", "igc", "bnk", "rcb", "txt", "ter", "tga", "xml", "das", "rx2", "hkx",
              "lua", "ast", "vxd"]
TEXT_LIMITS = {"name": 64, "author": 64, "version": 32, "description": 1024, "game": 64, "category": 32}


def valid_id(n):
    if not n or len(n) > 64 or n[0] == ".":
        return False
    return all(c.isascii() and (c.isalnum() or c in "_-.") for c in n)


def safe_rel(p):
    if not isinstance(p, str) or not p:
        return ""
    if p[0] in "/\\" or ":" in p:
        return ""
    parts = [c for c in p.replace("\\", "/").split("/") if c]
    if not parts or any(c in (".", "..") for c in parts):
        return ""
    return "/".join(parts)


def fnv1a64_upper(name):
    h = 0xCBF29CE484222325
    for c in name.upper().encode("latin1"):
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def asset_prefix(mod_id):
    """Globally scoped loose-asset names for additive character packs."""
    return "SRM_%016x_" % fnv1a64_upper(mod_id)


# ------------------------------------------------------------------ containers (top level)
def read_top_entry(path, entry):
    """-> bytes of one top-level entry of a BGFA container (by name, or #<16 hex> id)."""
    with open(path, "rb") as f:
        h = f.read(0x40)
        if h[:8] != b"BGFA1.05":
            raise ValueError("not a BGFA1.05 container")
        count, toc_off = struct.unpack_from("<II", h, 0x0C)
        wA, wId, wOff, wSize, wUnc, shift = h[0x21:0x27]
        ntags, namelen = struct.unpack_from("<II", h, 0x28)
        rec = 1 + wA + wId + wOff + wSize + wUnc + namelen
        f.seek(toc_off)
        toc = f.read(4 * ntags + rec * count)
        by_id = entry.startswith("#")
        want = int(entry[1:], 16) if by_id else fnv1a64_upper(entry)
        mask = (1 << (8 * wId)) - 1
        for i in range(count):
            r = toc[4 * ntags + rec * i: 4 * ntags + rec * (i + 1)]
            q = 1 + wA
            eid = int.from_bytes(r[q:q + wId], "little"); q += wId
            off = int.from_bytes(r[q:q + wOff], "little") << shift; q += wOff
            size = int.from_bytes(r[q:q + wSize], "little"); q += wSize + wUnc
            hit = eid == (want & mask)
            if not by_id and namelen and not hit:
                hit = r[q:q + namelen].split(b"\0")[0].decode("latin1").lower() == entry.lower()
            if hit:
                f.seek(off)
                return f.read(size)
    raise KeyError("no entry %r in %s" % (entry, path))


def toc_sha256(path):
    with open(path, "rb") as fh:
        head = fh.read(0x40)
        toc_off, toc_len = struct.unpack_from("<I", head, 0x10)[0], struct.unpack_from("<I", head, 0x18)[0]
        fh.seek(toc_off)
        return hashlib.sha256(head + fh.read(toc_len)).hexdigest()


def fingerprint(disc_root):
    data = os.path.join(disc_root, "DATA")
    names = sorted((f for f in os.listdir(data) if f.upper().endswith(".AST") and os.path.isfile(os.path.join(data, f))),
                   key=lambda f: f.upper())
    h = hashlib.sha256()
    for f in names:
        p = os.path.join(data, f)
        size = os.path.getsize(p)
        with open(p, "rb") as fh:
            head = fh.read(0x40)
            toc = b""
            if len(head) == 0x40 and head[:8] == b"BGFA1.05":
                fh.seek(struct.unpack_from("<I", head, 0x10)[0])
                toc = fh.read(struct.unpack_from("<I", head, 0x18)[0])
        h.update(f.upper().encode("latin1") + b"\0" + struct.pack("<Q", size) + hashlib.sha256(head + toc).digest())
    return "%s:%s" % (GAME_TAG, h.hexdigest()[:16])


# ------------------------------------------------------------------ mods
class Mod:
    def __init__(self, folder, path, is_zip):
        self.folder, self.path, self.is_zip = folder, path, is_zip
        self.id = folder[:-4] if is_zip and folder.lower().endswith(".zip") else folder
        self.root = None
        self.legacy = True
        self.name = self.author = self.version = self.description = self.game = ""
        self.preview = ""
        self.category = "other"
        self.priority = 0
        self.default_enabled = True
        self.requires, self.conflicts, self.patches = [], [], []
        self.costumes = []
        self.status, self.reason = "ok", ""
        self.enabled = True
        self.mounted = False

    def fail(self, st, why):
        if self.status == "ok":
            self.status, self.reason = st, why


def _zip_root(names):
    tops, file_at_root, modjson = [], False, False
    for n in names:
        if not n:
            continue
        if n.lower() == "mod.json":
            modjson = True
        top = n.split("/")[0]
        if "/" not in n:
            file_at_root = True
        if top not in tops:
            tops.append(top)
    if modjson or file_at_root or len(tops) != 1:
        return ""
    return tops[0] + "/"


def extract_zip(path, dst):
    with zipfile.ZipFile(path) as z:
        infos = z.infolist()
        root = _zip_root([i.filename for i in infos])
        for i in infos:
            if not i.filename.startswith(root) or i.filename.endswith("/"):
                continue
            rel = safe_rel(i.filename[len(root):])
            if not rel:
                raise ValueError("unsafe path %r in the zip" % i.filename)
            if i.flag_bits & 1:
                raise ValueError("%r is encrypted" % i.filename)
            if i.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                raise ValueError("%r uses compression method %d" % (i.filename, i.compress_type))
            out = os.path.join(dst, rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as fh:
                fh.write(z.read(i))


def _gen_list(j, key, root):
    out = []
    v = j.get(key, [])
    if not isinstance(v, list):
        raise ValueError("%s must be a list" % key)
    for x in v:
        a = safe_rel(x.get("asset")) if isinstance(x, dict) else ""
        p = safe_rel(x.get("patch")) if isinstance(x, dict) else ""
        base = x.get("base", "") if isinstance(x, dict) else ""
        bf, _, be = base.partition(":") if isinstance(base, str) else ("", "", "")
        bf = safe_rel(bf)
        if not a or not p or not bf or not be or "/" in be or not os.path.isfile(os.path.join(root, p)):
            raise ValueError("%s: entry %r needs asset, base \"<file>:<top-level entry>\" and an existing patch"
                             % (key, x.get("asset") if isinstance(x, dict) else x))
        out.append({"asset": a, "base_file": bf, "base_entry": be, "patch": os.path.join(root, p)})
    return out


def parse_manifest(m):
    mj = os.path.join(m.root, "mod.json")
    if not os.path.isfile(mj):
        return
    m.legacy = False
    try:
        j = json.loads(open(mj, "rb").read().decode("utf-8-sig"))
    except (ValueError, UnicodeDecodeError) as ex:
        return m.fail("bad-manifest", "mod.json: %s" % ex)
    if not isinstance(j, dict):
        return m.fail("bad-manifest", "mod.json is not an object")
    if "format" in j and (j["format"] != 1 or isinstance(j["format"], bool)):   # optional
        return m.fail("bad-manifest", "mod.json: format must be 1")
    for k in ["id"] + list(TEXT_LIMITS):
        if k in j:
            v = j[k]
            if not isinstance(v, str):
                return m.fail("bad-manifest", "mod.json: %s must be a string" % k)
            if k in TEXT_LIMITS and len(v.encode("utf-8")) > TEXT_LIMITS[k]:
                return m.fail("bad-manifest", "mod.json: %s is longer than %d bytes" % (k, TEXT_LIMITS[k]))
    m.id = j.get("id", m.id)
    m.costumes = []
    costumes = j.get("costumes", [])
    if not isinstance(costumes, list) or len(costumes) > 256:
        return m.fail("bad-manifest", "mod.json: costumes must be an array of at most 256 additions")
    ids, entities = set(), set()
    for costume in costumes:
        local = costume.get("id") if isinstance(costume, dict) else None
        name = costume.get("name") if isinstance(costume, dict) else None
        entity = costume.get("entity") if isinstance(costume, dict) else None
        if (not isinstance(local, str) or not valid_id(local) or
                not isinstance(name, str) or not name or len(name.encode("utf-8")) > 128 or
                not isinstance(entity, str) or not valid_id(entity) or
                not entity.lower().startswith(asset_prefix(m.id).lower()) or
                local.lower() in ids or entity.lower() in entities):
            return m.fail("bad-manifest", "mod.json: costumes need unique ids/names and entity stems in " +
                          asset_prefix(m.id) + " (no retail replacements)")
        ids.add(local.lower())
        entities.add(entity.lower())
        extra = {key: costume.get(key, "") for key in ("description", "thumbnail", "fullsize")}
        for key, value in extra.items():
            limit = 512 if key == "description" else 27
            if not isinstance(value, str):
                return m.fail("bad-manifest", "mod.json: " + key + " must be a string")
            if len(value.encode("utf-8")) > limit:
                return m.fail("bad-manifest", "mod.json: %s is longer than %d bytes" % (key, limit))
            if key != "description" and value and (not valid_id(value) or "." in value or
                    not value.lower().startswith(asset_prefix(m.id).lower())):
                return m.fail("bad-manifest", "mod.json: costume previews need scoped Apt movie stems (at most 27 characters)")
        project = costume.get("animation_project", "")
        if not isinstance(project, str):
            return m.fail("bad-manifest", "mod.json: animation_project must be a string")
        if len(project.encode("utf-8")) > 63:
            return m.fail("bad-manifest", "mod.json: animation_project is longer than 63 bytes")
        if project and (not valid_id(project) or "." in project or
                        not project.lower().startswith(asset_prefix(m.id).lower())):
            return m.fail("bad-manifest", "mod.json: animation_project needs an own-pack scoped resource stem (at most 63 characters)")
        extra["animation_project"] = project
        # own animation banks (kernel/mod_manager.cpp): SupermanAnims bank name -> a bank file in
        # the pack outside DATA; the family id is the content hash of the whole set
        banks = costume.get("animation_banks")
        if banks is not None:
            import costume_data
            if project or not isinstance(banks, dict) or not 1 <= len(banks) <= 6:
                return m.fail("bad-manifest", "mod.json: costume %s animation_banks: an object naming 1..6 SupermanAnims banks "
                              "(SUPERMAN_FLIGHT, ...) and their files" % local)
            digests, paths = [], []
            for bank, rel in banks.items():
                up = bank.upper()
                p = safe_rel(rel) if isinstance(rel, str) else ""
                blob = b""
                if p and not p.lower().startswith("data/") and os.path.isfile(os.path.join(m.root, p)):
                    with open(os.path.join(m.root, p), "rb") as f:
                        blob = f.read()
                if (up not in costume_data.SUPERMAN_BANKS or any(d[0] == up for d in digests) or
                        not costume_data.valid_superman_bank(blob)):
                    return m.fail("bad-manifest", "mod.json: costume %s animation_banks: '%s' must be a SupermanAnims bank "
                                  "name mapped to a bank file in the mod, outside DATA" % (local, bank))
                digests.append((up, hashlib.sha256(blob).hexdigest()))
                paths.append((up, os.path.join(m.root, p)))
            identity = "".join("%s\n%s\n" % d for d in sorted(digests))
            extra["banks"] = paths
            extra["bank_family"] = "banks_" + hashlib.sha256(identity.encode("ascii")).hexdigest()[:16]
        m.costumes.append({"id": m.id.lower() + "/" + local.lower(), "local_id": local,
                           "name": name, "source": m.id, "entity": entity, **extra})
    m.name, m.author, m.version = j.get("name", ""), j.get("author", ""), j.get("version", "")
    m.description, m.game = j.get("description", ""), j.get("game", "")
    m.category = (j.get("category", "") or ("characters" if m.costumes else "other")).lower()
    if m.category not in {"characters", "missions", "worlds", "gameplay", "visuals", "audio", "interface", "other"}:
        return m.fail("bad-manifest", "mod.json: unknown category '%s'" % m.category)
    for k in ("requires", "conflicts"):
        v = j.get(k, [])
        if not isinstance(v, list) or not all(isinstance(x, str) and valid_id(x) for x in v):
            return m.fail("bad-manifest", "mod.json: %s must be a list of mod ids" % k)
        setattr(m, k, v)
    try:
        m.patches = _gen_list(j, "patches", m.root)
    except ValueError as ex:
        return m.fail("bad-manifest", str(ex))
    if "priority" in j:
        if not isinstance(j["priority"], int) or isinstance(j["priority"], bool):
            return m.fail("bad-manifest", "mod.json: priority must be an integer")
        m.priority = j["priority"]
    if "enabled" in j:
        if not isinstance(j["enabled"], bool):
            return m.fail("bad-manifest", "mod.json: enabled must be true/false")
        m.default_enabled = j["enabled"]
    if j.get("preview"):
        p = safe_rel(j["preview"]) if isinstance(j["preview"], str) else ""
        if not p or not os.path.isfile(os.path.join(m.root, p)):
            return m.fail("bad-manifest", "mod.json: preview %r is not a file in the mod" % j["preview"])
        m.preview = os.path.join(m.root, p)


def load_mod(path, is_zip, cache_dir):
    m = Mod(os.path.basename(path), path, is_zip)
    if is_zip:
        st = os.stat(path)
        dst = os.path.join(cache_dir, "zip", "%s-%d-%d" % (m.id, st.st_size, st.st_mtime_ns // 100 + 116444736000000000))
        if not os.path.isfile(os.path.join(dst, ".complete")):
            tmp = dst + ".tmp"
            try:
                extract_zip(path, tmp)
            except (ValueError, zipfile.BadZipFile, OSError, EOFError) as ex:
                m.fail("bad-zip", str(ex))
                return m
            open(os.path.join(tmp, ".complete"), "w").write("ok\n")
            if os.path.isdir(dst):
                import shutil
                shutil.rmtree(dst)
            os.replace(tmp, dst)
        m.root = dst
    else:
        m.root = path
    parse_manifest(m)
    if not m.name:
        m.name = m.id
    if m.status == "ok" and not valid_id(m.id):
        m.fail("bad-manifest", "id '%s' is not a plain name (letters, digits, _ - .)" % m.id)
    return m


def parse_enabled_txt(text):
    out = []
    for i, line in enumerate(text.split("\n")):
        if i == 0 and line.startswith("﻿"):
            line = line[1:]
        line = line.split("#", 1)[0].strip()
        if line and line.lower() not in [x.lower() for x in out]:
            out.append(line)
    return out


def state_json(order, disabled, costume="", disabled_costumes=()):
    d = [x for x in order if x.lower() in disabled]
    return '{\n  "format": 1,\n  "order": [%s],\n  "disabled": [%s],\n  "costume": %s,\n  "disabled_costumes": [%s]\n}\n' % (
        ", ".join(json.dumps(x) for x in order), ", ".join(json.dumps(x) for x in d), json.dumps(costume),
        ", ".join(json.dumps(x) for x in sorted(disabled_costumes)))


# ------------------------------------------------------------------ port features
def port_feature_dir(content_root, exe_dir=None):
    for c in ([os.path.join(exe_dir, "port_features")] if exe_dir else []) + [os.path.join(content_root, "data", "port_features")]:
        if os.path.isdir(c):
            return c
    return None


class Feature:
    def __init__(self, fid, d):
        self.id, self.dir = fid, d
        self.name = self.description = self.option = ""
        self.assets = []
        self.status, self.reason = "ok", ""
        self.on = self.applied = False


def load_features(content_root, disc_root, exe_dir=None):
    out = []
    pf = port_feature_dir(content_root, exe_dir)
    if not pf:
        return out
    for d in sorted((x for x in os.listdir(pf) if os.path.isdir(os.path.join(pf, x))), key=str.lower):
        sd = os.path.join(pf, d)
        fj = os.path.join(sd, "feature.json")
        if not os.path.isfile(fj):
            continue
        f = Feature(d, sd)
        out.append(f)
        try:
            j = json.loads(open(fj, "rb").read().decode("utf-8-sig"))
            f.id = j.get("id", d)
            f.name = j.get("name", "") or f.id
            f.description = j.get("description", "")
            f.option = j.get("option") or ""
            f.assets = _gen_list(j, "assets", sd)
        except (ValueError, AttributeError) as ex:
            f.status, f.reason = "bad-manifest", "feature.json: %s" % ex
            continue
        for rel, pin in j.get("base", {}).items():
            p = os.path.join(disc_root, safe_rel(rel))
            if not os.path.isfile(p):
                f.status, f.reason = "wrong-game", "%s is missing or unreadable" % rel
            elif os.path.getsize(p) != pin.get("size") or toc_sha256(p) != pin.get("toc_sha256"):
                f.status, f.reason = "wrong-game", "%s is not the USA retail file this feature was made for" % rel
            if f.status != "ok":
                break
    return out


def generate_assets(layer_id, assets, disc_root, cache_dir):
    """-> the mirror root holding the generated assets (the exe's generateAssets)."""
    key = hashlib.sha256()
    pbytes = []
    for a in assets:
        b = open(a["patch"], "rb").read()
        pbytes.append(b)
        key.update(("%s|%s|%s|" % (a["asset"], a["base_file"], a["base_entry"])).encode("utf-8"))
        key.update(b)
    tag = "%s-%s" % (layer_id, key.hexdigest()[:16])
    d = os.path.join(cache_dir, "gen", tag)
    if os.path.isfile(os.path.join(d, ".complete")):
        return d
    tmp = d + ".tmp"
    for a, b in zip(assets, pbytes):
        base_path = os.path.join(disc_root, a["base_file"])
        base = read_top_entry(base_path, a["base_entry"])
        with open(base_path, "rb") as bf:
            def file_read(off, n, bf=bf):
                bf.seek(off)
                return bf.read(n)
            out = srpatch.apply(base, b, file_read)
        o = os.path.join(tmp, a["asset"])
        os.makedirs(os.path.dirname(o), exist_ok=True)
        open(o, "wb").write(out)
    open(os.path.join(tmp, ".complete"), "w").write("ok\n")
    os.replace(tmp, d)
    return d


# ------------------------------------------------------------------ the boot decision
def boot(content_root, mods_env=None, port_env=None, automated=False, custom_frontend_env=None,
         exe_dir=None, options=None, write_state=None, log=print, startup_costume=None):
    """kernel/mod_manager.cpp bootLocked, step by step. -> dict(mods, features, layers,
    conflicts, hidden, source)."""
    disc = os.path.join(content_root, DISC_DIR)
    mods_dir = os.path.join(content_root, "mods")
    cache = os.path.join(mods_dir, ".cache")
    if mods_env is None:
        src = "none" if automated else "state"
    elif mods_env in ("", "0"):
        src = "none"
    elif mods_env in ("@player", "@enabled.txt"):
        src = "state"
    else:
        src = "list"
    explicit = [x.strip() for x in mods_env.replace(",", ";").split(";") if x.strip()] if src == "list" else []
    if port_env in ("0", "1"):
        port_default = port_env == "1"
    else:
        port_default = not automated and mods_env != "0"
    forced_off = port_env == "0"
    if write_state is None:
        write_state = not automated and mods_env is None
    feats = load_features(content_root, disc, exe_dir)
    reserved = {f.id.lower() for f in feats} | {"retail"}  # retail/<n> = the retail Fortress suits
    for f in feats:
        on = port_default or (not forced_off and any(x.lower() == f.id.lower() for x in explicit))
        if custom_frontend_env is not None and f.id.lower() == "custom_frontend":
            if custom_frontend_env == "1" and not forced_off:
                on = True
            if custom_frontend_env == "0":
                on = False
        f.on = on
        if f.status == "ok" and not on:
            f.status, f.reason = "off", "port features off in this run"
        if f.status == "ok" and f.option:
            if (options or {}).get(f.option, 1) != 1:
                f.status, f.reason = "disabled", "option %s is off" % f.option
    found = []
    if src != "none" and os.path.isdir(mods_dir):
        for n in sorted(os.listdir(mods_dir), key=str.lower):
            if n.startswith((".", "@")):
                continue
            p = os.path.join(mods_dir, n)
            if os.path.isdir(p):
                found.append(load_mod(p, False, cache))
            elif n.lower().endswith(".zip") and os.path.isfile(p):
                found.append(load_mod(p, True, cache))
    by_id = {}
    for m in found:
        if m.id.lower() in by_id:
            m.fail("duplicate", "another mod has the id %s" % m.id)
        else:
            by_id[m.id.lower()] = m
    order, disabled, selected_costume, disabled_costumes = [], set(), "", set()
    if src == "state":
        source = "mods/mods.json"
        sj, et = os.path.join(mods_dir, "mods.json"), os.path.join(mods_dir, "enabled.txt")
        if os.path.isfile(sj):
            try:
                j = json.loads(open(sj, "rb").read().decode("utf-8-sig"))
                selected_costume = j.get("costume", "") if isinstance(j.get("costume", ""), str) else ""
                selected_costume = selected_costume.lower()
                exclusions = j.get("disabled_costumes", [])
                if isinstance(exclusions, list):
                    disabled_costumes = {x.lower() for x in exclusions if isinstance(x, str) and
                                        len(x.split("/")) == 2 and all(valid_id(part) for part in x.split("/"))}
                order = [x for x in j.get("order", []) if isinstance(x, str)]
                disabled = {x.lower() for x in j.get("disabled", []) if isinstance(x, str)}
            except ValueError as ex:
                log("[MODS] error: mods/mods.json unreadable (%s): default order, every mod on" % ex)
                source = "mods/mods.json (unreadable)"
        else:
            fresh = sorted([m for m in found if m.status != "duplicate"], key=lambda m: -m.priority)
            if os.path.isfile(et):
                order = parse_enabled_txt(open(et, "rb").read().decode("utf-8", "replace"))
                listed = {x.lower() for x in order}
                for m in fresh:
                    if m.id.lower() not in listed:
                        order.append(m.id)
                        disabled.add(m.id.lower())
                source = "mods/enabled.txt (migrated)"
                if write_state:
                    open(sj + ".tmp", "w", encoding="utf-8", newline="\n").write(state_json(order, disabled))
                    os.replace(sj + ".tmp", sj)
                    os.replace(et, et + ".migrated")
                    log("[MODS] migrated mods/enabled.txt -> mods/mods.json (enabled.txt renamed enabled.txt.migrated)")
            else:
                for m in fresh:
                    order.append(m.id)
                    if not m.default_enabled:
                        disabled.add(m.id.lower())
                source = "default order (no mods/mods.json yet)"
        listed = {x.lower() for x in order}
        for m in found:
            if m.status == "duplicate" or m.id.lower() in listed:
                continue
            order.append(m.id)
            listed.add(m.id.lower())
            if not m.default_enabled:
                disabled.add(m.id.lower())
    elif src == "list":
        source = "SR_MODS"
        order = [x for x in explicit if x.lower() not in reserved]
    else:
        source = "none"
    mods, used = [], set()
    for x in order:
        m = by_id.get(x.lower())
        if m is None:
            if src == "list":
                log("[MOD] error: mod '%s' is enabled but not in mods/ - skipped" % x)
            continue
        if x.lower() in used:
            continue
        used.add(x.lower())
        m.enabled = m.id.lower() not in disabled
        mods.append(m)
    if src == "state":
        mods += [m for m in found if m.status == "duplicate"]
    for m in mods:
        if m.id.lower() in reserved:
            m.fail("reserved", "%s is built in now (port feature); this copy is ignored" % m.id)
        if not m.enabled:
            m.fail("disabled", "switched off")
    fp = None
    for m in mods:
        if m.status == "ok" and m.game:
            fp = fp or fingerprint(disc)
            if not ((m.game == GAME_TAG and fp == "%s:%s" % (GAME_TAG, USA_RETAIL_FINGERPRINT)) or m.game == fp):
                m.fail("wrong-game", "made for %s; this data is %s" % (m.game, fp))
    changed = True
    while changed:
        changed = False
        live = {m.id.lower() for m in mods if m.status == "ok"}
        for m in mods:
            if m.status == "ok":
                miss = [r for r in m.requires if r.lower() not in live]
                if miss:
                    m.fail("missing-dependency", "needs %s" % miss[0])
                    changed = True
        for a in range(len(mods)):
            if changed:
                break
            for b in range(a + 1, len(mods)):
                hi, lo = mods[a], mods[b]
                if hi.status != "ok" or lo.status != "ok":
                    continue
                if lo.id.lower() in [x.lower() for x in hi.conflicts] or hi.id.lower() in [x.lower() for x in lo.conflicts]:
                    lo.fail("declared-conflict", "conflicts with %s" % hi.id)
                    changed = True
    port_layers = []
    for f in feats:
        if f.status != "ok":
            continue
        try:
            d = generate_assets("port-" + f.id, f.assets, disc, cache)
        except (srpatch.PatchError, KeyError, ValueError, OSError) as ex:
            f.status, f.reason = "bad-patch", str(ex)
            continue
        f.applied = True
        port_layers.append({"id": f.id, "name": f.name, "port": True, "roots": [d]})
    layers = []
    for m in mods:
        if m.status != "ok":
            continue
        roots = []
        if m.patches:
            try:
                roots.append(generate_assets("mod-" + m.id, m.patches, disc, cache))
            except (srpatch.PatchError, KeyError, ValueError, OSError) as ex:
                m.fail("bad-patch", str(ex))
                continue
        roots.append(m.root)
        m.mounted = True
        layers.append({"id": m.id, "name": m.name, "port": False, "roots": roots})
    layers += port_layers
    conflicts, hidden = analyse_conflicts(layers)
    costumes = sorted((dict(c) for m in mods if m.mounted and m.status == "ok" for c in m.costumes
                       if c["id"] not in disabled_costumes), key=lambda c: c["id"])
    for i, c in enumerate(costumes):
        c["slot"] = 5 + i
    if startup_costume:
        selected_costume = startup_costume.lower()
    registry_ready = False
    if costumes:
        try:
            import costume_data
            base, overridden = None, []
            for layer in layers:
                for path in layer["roots"]:
                    for rel, full in layer_files(path):
                        if (rel.lower().startswith("data/_workinprogress/") and
                                os.path.basename(full).lower() == "supermenactorworldflowattributes.das" and
                                os.path.normcase(os.path.abspath(full)) not in hidden):
                            if base is None:
                                with open(full, "rb") as source:
                                    base = source.read()
                            overridden.append(os.path.normcase(os.path.abspath(full)))
            if base is None:
                bundle = read_top_entry(os.path.join(disc, "DATA", "objtiv.AST"), "SUPERMENACTORWORLDFLOW")
                script = costume_data.nested_entry(bundle, "SUPERMENACTORWORLDFLOW_SCRIPT")
                base = costume_data.nested_entry(script, "SUPERMENACTORWORLDFLOWATTRIBUTES")
            # the worn suit at boot (SRWornCostume): retail/<1..3> or a mounted <mod>/<costume>,
            # else 0 (the retail default) - kernel/mod_manager.cpp bootLocked
            startup_slot = 0
            if selected_costume:
                m = re.fullmatch(r"retail/([1-3])", selected_costume.lower())
                startup_slot = int(m.group(1)) if m else 0
                startup_slot = next((c["slot"] for c in costumes if c["id"] == selected_costume), startup_slot)
            # the session's animation family = the worn suit's: its own banks go into the winning
            # baseworl.AST (kernel/mod_manager.cpp bootLocked); a failure keeps the retail family
            # and the retail default suit
            session_family, patched_baseworld = costume_data.RETAIL_FAMILY, None
            worn = next((c for c in costumes if c["slot"] == startup_slot and c.get("banks")), None)
            if worn:
                base_path = os.path.join(disc, "DATA", "baseworl.AST")
                for layer in layers:
                    hit = next((os.path.join(r, "DATA", "baseworl.AST") for r in layer["roots"]
                                if os.path.isfile(os.path.join(r, "DATA", "baseworl.AST"))), None)
                    if hit:
                        base_path = hit
                        break
                try:
                    with open(base_path, "rb") as f:
                        whole = f.read()
                    blobs = []
                    for bank, path in worn["banks"]:
                        with open(path, "rb") as f:
                            blobs.append((bank, f.read()))
                    patched_baseworld = costume_data.patch_baseworld_banks(whole, blobs)
                    session_family = worn["bank_family"]
                except (ValueError, OSError, KeyError) as ex:
                    log("[COSTUME] worn suit %s: own animation banks not applied (%s); Superman wears the retail default"
                        % (worn["id"], ex))
                    startup_slot = 0
            composed = costume_data.compose_attributes(base, costumes, startup_slot, session_family)
            startup_scripts, retail_scripts, actor_scripts = {}, None, None
            if True:
                for entry in ("BASEWORLDFLOWFSM_S_7E412353", "BASEWORLDFLOWFSM_S_4982930A",
                              "SUPERMENACTORWORLDFLOWFSM_S_60265281"):
                    lua = None
                    for layer in layers:
                        for path in layer["roots"]:
                            for rel, full in layer_files(path):
                                if (rel.lower().startswith("data/_workinprogress/") and
                                        os.path.basename(full).lower() == entry.lower() + ".lua" and
                                        os.path.normcase(os.path.abspath(full)) not in hidden):
                                    if lua is None:
                                        with open(full, "rb") as source:
                                            lua = source.read()
                                    overridden.append(os.path.normcase(os.path.abspath(full)))
                    if lua is None:
                        if entry == "SUPERMENACTORWORLDFLOWFSM_S_60265281":
                            if actor_scripts is None:
                                actor_bundle = read_top_entry(os.path.join(disc, "DATA", "objtiv.AST"), "SUPERMENACTORWORLDFLOW")
                                actor_scripts = costume_data.nested_entry(actor_bundle, "SUPERMENACTORWORLDFLOW_SCRIPT")
                            lua = costume_data.nested_entry(actor_scripts, entry)
                        else:
                            if retail_scripts is None:
                                retail_scripts = read_top_entry(os.path.join(disc, "DATA", "baseworl.AST"), "BASEWORLDFLOW_SCRIPT")
                            lua = costume_data.nested_entry(retail_scripts, entry)
                    startup_scripts[entry + ".lua"] = costume_data.compose_switch_lua(lua, entry)
            if any(c.get("animation_project") for c in costumes):
                if os.environ.get("SR_COSTUME_PROJECT_PRELOADS") != "1":
                    raise ValueError("own animation projects need SR_COSTUME_PROJECT_PRELOADS=1 (runtime validation pending)")
                preloads = None
                for layer in layers:
                    for path in layer["roots"]:
                        for rel, full in layer_files(path):
                            if (rel.lower().startswith("data/_workinprogress/") and
                                    os.path.basename(full).lower() == "baseworldflowpreloads.xml" and
                                    os.path.normcase(os.path.abspath(full)) not in hidden):
                                if preloads is None:
                                    with open(full, "rb") as source:
                                        preloads = source.read()
                                overridden.append(os.path.normcase(os.path.abspath(full)))
                if preloads is None:
                    other = read_top_entry(os.path.join(disc, "DATA", "baseworl.AST"), "BASEWORLDFLOW_OTHER")
                    preloads = costume_data.nested_entry(other, "BASEWORLDFLOWPRELOADS")
                startup_scripts["BASEWORLDFLOWPRELOADS.xml"] = costume_data.compose_preloads(preloads, costumes)
            identity = composed + b"".join(name.encode("ascii") + b"\0" + data for name, data in sorted(startup_scripts.items()))
            if patched_baseworld is not None:
                identity += b"DATA/baseworl.AST" + hashlib.sha256(patched_baseworld).hexdigest().encode("ascii")
            directory = os.path.join(cache, "gen", "costumes-" + hashlib.sha256(identity).hexdigest())
            outputs = {"SUPERMENACTORWORLDFLOWATTRIBUTES.das": composed, **startup_scripts}
            if patched_baseworld is not None:
                filename = os.path.join(directory, "DATA", "baseworl.AST")
                os.makedirs(os.path.dirname(filename), exist_ok=True)
                with open(filename + ".tmp", "wb") as target:
                    target.write(patched_baseworld)
                os.replace(filename + ".tmp", filename)
            for name, data in outputs.items():
                filename = os.path.join(directory, "DATA", "_workinprogress", "_costume_registry", name)
                os.makedirs(os.path.dirname(filename), exist_ok=True)
                with open(filename + ".tmp", "wb") as target:
                    target.write(data)
                os.replace(filename + ".tmp", filename)
            layers.insert(0, {"id": "_costume_registry", "name": "Combined costume registry", "port": True, "roots": [directory]})
            hidden.update(overridden)
            registry_ready = True
        except (ValueError, KeyError, OSError, srvfs.VfsError) as ex:
            log("[COSTUME] registry unavailable: " + str(ex))
    return {"mods": mods, "features": feats, "layers": layers, "conflicts": conflicts, "hidden": hidden,
            "source": source, "order": order, "disabled": disabled,
            "costumes": costumes, "costume_registry_ready": registry_ready, "selected_costume": selected_costume,
            "disabled_costumes": disabled_costumes}


def layer_files(root):
    """mirror-root files as (rel, host path), skipping '.'/'@' names and the root mod.json
    (kernel/mod_manager.cpp: recursive_directory_iterator order is not sorted, the result is)."""
    out = []
    for dp, dn, fns in os.walk(root):
        dn[:] = [d for d in dn if not d.startswith((".", "@"))]
        for fn in fns:
            if fn.startswith((".", "@")):
                continue
            full = os.path.join(dp, fn)
            rel = os.path.relpath(full, root).replace("\\", "/")
            if rel.lower() == "mod.json":
                continue
            out.append((rel, full))
    return out


def analyse_conflicts(layers):
    path_owner, stem_owner = {}, {}
    conflicts, hidden = [], set()
    for li, L in enumerate(layers):
        mine = set()
        for r in L["roots"]:
            for rel, full in sorted(layer_files(r)):
                low = rel.lower()
                if low in mine:
                    continue
                mine.add(low)
                po = path_owner.get(low)
                if po is not None and po != li:
                    W = layers[po]
                    conflicts.append((rel, W["id"], L["id"]))
                    continue
                path_owner[low] = li
                if not low.startswith("data/_workinprogress/"):
                    continue
                fn = rel.rsplit("/", 1)[-1]
                if "." not in fn or fn.rsplit(".", 1)[1].lower() not in LOOSE_EXTS:
                    continue
                stem = fn.rsplit(".", 1)[0].lower()
                if stem.startswith("__asset__"):
                    stem = "#" + stem[9:]
                so = stem_owner.get(stem)
                if so is not None and so != li:
                    conflicts.append(("asset %s (%s)" % (fn.rsplit(".", 1)[0], rel), layers[so]["id"], L["id"]))
                    hidden.add(os.path.normcase(os.path.abspath(full)))
                    continue
                stem_owner[stem] = li
    return conflicts, hidden


# ------------------------------------------------------------------ simulate
def union_listing(disc_dir_path, rel_dir, layers, hidden):
    """the overlay's listing of one folder: disc entries (disc order) then names only layers
    provide (highest layer first, each sorted), hidden conflict losers removed."""
    out, seen = [], set()
    if os.path.isdir(disc_dir_path):
        for n in os.listdir(disc_dir_path):
            out.append((n, os.path.isdir(os.path.join(disc_dir_path, n))))
            seen.add(n.lower())
    for L in layers:
        for r in L["roots"]:
            d = os.path.join(r, rel_dir)
            if not os.path.isdir(d):
                continue
            mine = []
            for n in os.listdir(d):
                if n.startswith((".", "@")) or (rel_dir == "" and n.lower() == "mod.json"):
                    continue
                full = os.path.join(d, n)
                if os.path.normcase(os.path.abspath(full)) in hidden:
                    continue
                mine.append((n, os.path.isdir(full)))
            for n, isdir in sorted(mine, key=lambda x: x[0].lower()):
                if n.lower() in seen:
                    continue
                seen.add(n.lower())
                out.append((n, isdir))
    return out


def game_sees(names, fixed, mask="*.*"):
    """FindFirst/FindNext over a listing: Xenia-faithful = every match; the old host
    packing = the first record of each 328-byte call, mask ignored."""
    def match(n):
        if mask in ("*", "*.*", ""):
            return True
        import fnmatch
        return fnmatch.fnmatch(n.lower(), mask.lower())
    if fixed:
        return [x for x in names if match(x[0])]
    seen, i = [], 0
    while i < len(names):
        seen.append(names[i])
        used = 0
        while i < len(names):
            sz = 64 + ((len(names[i][0]) + 3) & ~3)
            if used + sz > 328:
                break
            used += sz
            i += 1
    return seen


def simulate(content_root, layers, hidden, fixed):
    disc = os.path.join(content_root, DISC_DIR)
    reg, ignored = {}, []

    def scan(rel):
        names = union_listing(os.path.join(disc, rel), rel, layers, hidden)
        files = [x for x in game_sees(names, fixed, "*.*") if not x[1]]
        assets = game_sees(names, fixed, "__ASSET__*.*") if fixed else []
        for n, _ in files + [a for a in assets if not a[1]]:
            if "." not in n:
                ignored.append(rel + "/" + n)
                continue
            stem, ext = n.rsplit(".", 1)
            if ext.lower() not in LOOSE_EXTS:
                ignored.append(rel + "/" + n)
                continue
            key = ("#" + stem[9:].lower()) if stem.lower().startswith("__asset__") else stem.lower()
            reg.setdefault(key, []).append(rel + "/" + n)
        for n, isdir in game_sees(names, fixed, "*"):
            if isdir:
                scan(rel + "/" + n)
    scan("DATA/_workinprogress")
    return reg, sorted(set(ignored))


# ------------------------------------------------------------------ CLI
def _boot_from_args(a, write_state=False):
    return boot(a.root, mods_env=a.mods, port_env=a.port, automated=a.automated, write_state=write_state,
                log=lambda *x: None, startup_costume=a.costume)


def _cmd_list(a):
    r = _boot_from_args(a)
    print("# source: %s" % r["source"])
    for f in r["features"]:
        print("port  %-22s %-12s %s" % (f.id, f.status if not f.applied else "applied", f.reason))
    for i, m in enumerate(r["mods"]):
        print("%3d   %-22s %-18s %-24s %s" % (i, m.id, m.status, m.name, m.reason))
    for c in r["conflicts"]:
        print("conflict %s: %s wins over %s" % c)
    for c in r["costumes"]:
        print("costume %s slot=%d entity=%s" % (c["id"], c["slot"], c["entity"]))
    return 0


def _cmd_conflicts(a):
    r = _boot_from_args(a)
    for c in r["conflicts"]:
        print("%s: %s wins over %s" % c)
    print("%d conflict(s)" % len(r["conflicts"]))
    return 0


def _cmd_simulate(a):
    r = _boot_from_args(a)
    for fixed in (False, True):
        reg, ignored = simulate(a.root, r["layers"], r["hidden"], fixed)
        print("== %s host listing: %d loose asset(s) registered" % ("Xenia-faithful (SR_NTQDIR_XENIA=1)" if fixed else "old", len(reg)))
        for k in sorted(reg):
            print("   %-40s %s" % (k, ", ".join(reg[k])))
        if ignored:
            print("   not registered (no type for the extension): %s" % ", ".join(ignored))
    return 0


def _cmd_lint(a):
    p = os.path.abspath(a.mod)
    m = load_mod(p, p.lower().endswith(".zip"), os.path.join(a.root, "mods", ".cache"))
    print("id %s: %s %s" % (m.id, m.status, m.reason))
    if m.status != "ok":
        return 1
    rc = 0
    for rel, full in sorted(layer_files(m.root)):
        low = rel.lower()
        if low.startswith("data/_workinprogress/"):
            fn = rel.rsplit("/", 1)[-1]
            ext = fn.rsplit(".", 1)[1].lower() if "." in fn else ""
            if ext not in LOOSE_EXTS:
                print("  warning: %s: the engine ignores the extension .%s (registered: %s)" % (rel, ext, " ".join(LOOSE_EXTS)))
                rc = 1
            elif ext in ("apt", "const", "ini"):
                print("  warning: %s: not a loose type" % rel)
    for g in m.patches:
        try:
            base = read_top_entry(os.path.join(a.root, DISC_DIR, g["base_file"]), g["base_entry"])
            hdr, _ = srpatch.parse(open(g["patch"], "rb").read())
            ok = len(base) == hdr["base_size"] and hashlib.sha256(base).digest() == hdr["base_sha256"]
            print("  patch %s <- %s:%s %s" % (g["asset"], g["base_file"], g["base_entry"], "OK" if ok else "BASE MISMATCH"))
            rc |= 0 if ok else 1
        except (KeyError, ValueError, srpatch.PatchError, OSError) as ex:
            print("  patch %s: %s" % (g["asset"], ex))
            rc = 1
    return rc


def _cmd_order(a):
    mods_dir = os.path.join(a.root, "mods")
    r = boot(a.root, mods_env="@player", automated=True, write_state=False, log=lambda *x: None)
    order, disabled = list(r["order"]), set(r["disabled"])
    if a.set:
        want = [x.strip() for x in a.set.split(",") if x.strip()]
        order = want + [x for x in order if x.lower() not in [w.lower() for w in want]]
    for x in a.enable or []:
        disabled.discard(x.lower())
    for x in a.disable or []:
        disabled.add(x.lower())
    if a.set or a.enable or a.disable:
        sj = os.path.join(mods_dir, "mods.json")
        open(sj + ".tmp", "w", encoding="utf-8", newline="\n").write(
            state_json(order, disabled, r["selected_costume"], r["disabled_costumes"]))
        os.replace(sj + ".tmp", sj)
        print("wrote %s" % sj)
    for i, x in enumerate(order):
        print("%3d  %s%s" % (i, x, "  (off)" if x.lower() in disabled else ""))
    return 0


def _cmd_gen(a):
    r = _boot_from_args(a)
    for L in r["layers"]:
        if L["port"] or len(L["roots"]) > 1:
            src = L["roots"][0]
            for rel, full in sorted(layer_files(src)):
                dst = os.path.join(a.out, L["id"], rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                open(dst, "wb").write(open(full, "rb").read())
                print("%s %s %s" % (L["id"], rel, hashlib.sha256(open(full, "rb").read()).hexdigest()))
    return 0


def _cmd_pack(a):
    src = os.path.abspath(a.mod)
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED) as z:
        for dp, dn, fns in os.walk(src):
            dn[:] = [d for d in dn if not d.startswith(".")]
            for fn in fns:
                full = os.path.join(dp, fn)
                z.write(full, os.path.relpath(full, src).replace("\\", "/"))
    print("wrote %s" % a.out)
    return 0


def _cmd_fingerprint(a):
    print(fingerprint(os.path.join(a.root, DISC_DIR)))
    return 0


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="The mod framework's tooling (docs/MODS.md).")
    ap.add_argument("cmd", choices=["list", "lint", "order", "conflicts", "simulate", "gen", "pack", "fingerprint"])
    ap.add_argument("mod", nargs="?")
    ap.add_argument("--root", default=REPO)
    ap.add_argument("--out")
    ap.add_argument("--mods", default=None)
    ap.add_argument("--port", default=None)
    ap.add_argument("--costume", default=None, help="stable pack/skin id; startup content request, not an immediate swap")
    ap.add_argument("--automated", action="store_true")
    ap.add_argument("--set")
    ap.add_argument("--enable", action="append")
    ap.add_argument("--disable", action="append")
    a = ap.parse_args(argv)
    return {"list": _cmd_list, "lint": _cmd_lint, "order": _cmd_order, "conflicts": _cmd_conflicts,
            "simulate": _cmd_simulate, "gen": _cmd_gen, "pack": _cmd_pack, "fingerprint": _cmd_fingerprint}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
