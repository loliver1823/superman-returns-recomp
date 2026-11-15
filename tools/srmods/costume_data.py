"""Content-only reference for kernel/mod_costume_data.cpp (flat compiled DAS XML)."""
import struct

import srvfs

RETAIL_FAMILY = "retail"  # the retail suits' animation family (the disc's SupermanAnims banks)
SUPERMAN_BANKS = ("SUPERMAN_FLIGHT", "SUPERMAN_ONFOOT", "SUPERMAN_MELEEGROUND",
                  "SUPERMAN_SUPER_POWERS", "SUPERMAN_SUPERSPEED_ATTACKS", "SUPERMAN_MELEEAIR")


def nested_entry(data, name):
    container = srvfs.Bgfa(srvfs.Src.mem(data))
    key = srvfs.fnv1a64_upper(name)
    for entry in container.entries:
        if entry.id == key:
            return entry.data.read(0, entry.size)
    raise ValueError("nested BGFA entry missing: " + name)


def family(c):
    """kernel/mod_costume_data.cpp modCostumeFamily."""
    return c.get("bank_family") or c.get("animation_project") or RETAIL_FAMILY


def compose_attributes(data, additions, startup_slot=0, active_family=RETAIL_FAMILY):
    if len(data) < 44 or data[:8] != bytes.fromhex("05f5e0ff00000014"):
        raise ValueError("costume base must be compiled attribute XML")
    header = struct.unpack_from(">7I", data, 16)
    ne, ns, blob_len = header[0] >> 16, header[2] & 65535, header[4]
    blob = 44 + 12 * ns
    elements = blob + blob_len
    if header[1] != 44 or not ne or elements + 12 * ne > len(data):
        raise ValueError("compiled attribute XML bounds")
    strings = []
    for i in range(ns):
        length, off, _ = struct.unpack_from(">3I", data, 44 + i * 12)
        if off + length > blob_len:
            raise ValueError("compiled XML string bounds")
        text = data[blob + off:blob + off + length].decode("latin1")
        if "\0" in text:
            raise ValueError("compiled XML embedded NUL")
        strings.append(text)
    records, keys = [], set()
    for i in range(ne):
        off, count, name, end, _ = struct.unpack_from(">I4H", data, elements + i * 12)
        if name >= ns or off + 4 * count > len(data):
            raise ValueError("compiled XML element bounds")
        if i == 0:
            if strings[name] != "attributes" or end != ne - 1 or count:
                raise ValueError("unsupported attribute XML root")
            continue
        if strings[name] != "attribute" or end != i:
            raise ValueError("unsupported attribute XML tree")
        attrs = []
        for j in range(count):
            k, v = struct.unpack_from(">2H", data, off + j * 4)
            if k >= ns or v >= ns:
                raise ValueError("compiled XML attribute bounds")
            attrs.append((strings[k], strings[v]))
        for key, _ in attrs:
            if not key or not (key[0].isascii() and (key[0].isalpha() or key[0] == "_")) or not all(
                    c.isascii() and (c.isalnum() or c in "_.-") for c in key):
                raise ValueError("invalid XML field name")
        values = dict(attrs)
        if len(values) != len(attrs) or "key" not in values or values["key"] in keys:
            raise ValueError("duplicate or missing DAS key")
        keys.add(values["key"])
        records.append(attrs)
    if not {"SetCostumeNum", "Costume1", "Costume2", "Costume3", "Costume4"} <= keys:
        raise ValueError("retail costume entry missing")
    identities, entities = set(), set()
    for c in additions:
        key = "Costume" + str(c["slot"])
        if c["slot"] < 5 or key in keys or c["id"] in identities or c["entity"].lower() in entities:
            raise ValueError("duplicate or invalid costume addition")
        keys.add(key)
        identities.add(c["id"])
        entities.add(c["entity"].lower())
        records.append([("key", key), ("type", "String"), ("category", "0"), ("constant", "1"), ("value", c["entity"])])

    # iter-465 Fortress costumes: the animation family of every suit, the session's family, the
    # worn suit, the switcher's state (kernel/mod_costume_data.cpp modComposeCostumeAttributes,
    # the same records in the same order)
    if startup_slot:
        if startup_slot not in {1, 2, 3} | {c["slot"] for c in additions}:
            raise ValueError("startup costume slot is not present")
    for n in (1, 2, 3):
        records.append([("key", "SRCostumeFamily%d" % n), ("type", "String"), ("category", "0"),
                        ("constant", "1"), ("value", RETAIL_FAMILY)])
    for c in additions:
        records.append([("key", "SRCostumeFamily%d" % c["slot"]), ("type", "String"), ("category", "0"),
                        ("constant", "1"), ("value", family(c))])
    if {"SRWornCostume", "SRAppliedTo", "SRActiveFamily"} & keys:
        raise ValueError("costume switch attribute already present")
    records.append([("key", "SRWornCostume"), ("type", "Integer"), ("category", "0"), ("constant", "0"),
                    ("value", str(startup_slot))])
    records.append([("key", "SRAppliedTo"), ("type", "Integer"), ("category", "0"), ("constant", "0"), ("value", "0")])
    records.append([("key", "SRActiveFamily"), ("type", "String"), ("category", "0"), ("constant", "1"),
                    ("value", active_family)])

    def escape(text):
        subs = {"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;"}
        return "".join(subs.get(c, "&#%d;" % ord(c) if ord(c) >= 128 or ord(c) < 32 else c) for c in text)

    return ("<attributes>\n" + "".join("  <attribute" + "".join(' %s="%s"' % (k, escape(v)) for k, v in r) + "/>\n"
                                        for r in records) + "</attributes>\0").encode("ascii")


def compose_preloads(data, additions):
    """Compiled retail/winning preload tree, plus distinct own animation projects."""
    import srmods
    if (len(data) < 44 or len(data) > 16 * 1024 * 1024 or
            data[:8] != bytes.fromhex("05f5e0ff00000014")):
        raise ValueError("costume preloads need compiled preload XML")
    header = struct.unpack_from(">7I", data, 16)
    ne, ns, blob_len = header[0] >> 16, header[2] & 65535, header[4]
    blob = 44 + 12 * ns
    elements = blob + blob_len
    if header[1] != 44 or not 0 < ne <= 4096 or ns > 8192 or elements + 12 * ne > len(data):
        raise ValueError("compiled preload XML bounds")
    strings, string_budget = [], 0
    for i in range(ns):
        length, off, _ = struct.unpack_from(">3I", data, 44 + i * 12)
        string_budget += length
        if off + length > blob_len or length > 65536 or string_budget > 16 * 1024 * 1024:
            raise ValueError("preload string bounds")
        value = data[blob + off:blob + off + length].decode("latin1")
        if "\0" in value:
            raise ValueError("preload embedded NUL")
        strings.append(value)
    def escape(value):
        subs = {"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;"}
        return "".join(subs.get(c, "&#%d;" % ord(c) if ord(c) >= 128 or ord(c) < 32 else c) for c in value)
    nodes, budget = [], 0
    for i in range(ne):
        off, count, name, end, _ = struct.unpack_from(">I4H", data, elements + i * 12)
        if name >= ns or not i <= end < ne or off + count * 4 > len(data):
            raise ValueError("preload element bounds")
        values, attrs = {}, ""
        for j in range(count):
            k, v = struct.unpack_from(">2H", data, off + j * 4)
            if (k >= ns or v >= ns or not srmods.valid_id(strings[k]) or
                    not (strings[k][0].isascii() and (strings[k][0].isalpha() or strings[k][0] == "_")) or
                    strings[k] in values):
                raise ValueError("invalid preload attribute")
            values[strings[k]] = strings[v]
            attrs += ' %s="%s"' % (strings[k], escape(strings[v]))
            if len(attrs) > 16 * 1024 * 1024:
                raise ValueError("preload output too large")
        budget += len(attrs) + 64
        if budget > 16 * 1024 * 1024:
            raise ValueError("preload output too large")
        nodes.append((strings[name], end, attrs, values))
    if nodes[0][0] != "preloadDefs" or nodes[0][1] != ne - 1:
        raise ValueError("unsupported preload root")
    text, projects = "<preloadDefs" + nodes[0][2] + ">\n", {}
    i = 1
    while i < ne:
        name, end, attrs, values = nodes[i]
        if name != "preload" or "type" not in values:
            raise ValueError("unsupported preload group")
        text += "  <preload" + attrs + ">\n"
        for j in range(i + 1, end + 1):
            file_name, file_end, file_attrs, file_values = nodes[j]
            if file_name != "file" or file_end != j or "name" not in file_values:
                raise ValueError("unsupported preload file tree")
            if values["type"] == "ANIM":
                project = file_values["name"]
                if projects.setdefault(project.lower(), project) != project:
                    raise ValueError("case-ambiguous preload project")
            text += "    <file" + file_attrs + "/>\n"
        text += "  </preload>\n"
        i = end + 1
    extra = []
    for costume in additions:
        project = costume.get("animation_project", "")
        if not project:
            continue
        if (len(project) > 63 or not srmods.valid_id(project) or "." in project or
                not srmods.valid_id(costume["source"]) or
                not project.lower().startswith(srmods.asset_prefix(costume["source"]).lower())):
            raise ValueError("unscoped costume preload project")
        if project.lower() in projects:
            if projects[project.lower()] != project:
                raise ValueError("case-ambiguous preload project")
        else:
            projects[project.lower()] = project
            extra.append(project)
    if extra:
        text += '  <preload type="ANIM">\n'
        text += "".join('    <file name="%s"/>\n' % project for project in extra)
        text += "  </preload>\n"
    return (text + "</preloadDefs>\0").encode("ascii")


RETAIL_COSTUME_STATE = (
    b'local costumeNum = GetDASValue("SetCostumeNum")',
    b'SetDASValue("SetCostumeNum", 0)',
    b'local newCostume = GetDASValue("Costume" .. costumeNum)',
    b'local supermanID = GetIDFromBinIndex("SUPERMAN", 0)',
    b'ReplaceEntityVisRep(supermanID, newCostume);',
)
# kernel/mod_costume_data.cpp kCostumeSwitchLua, byte for byte
COSTUME_SWITCH_LUA = b'function main(mode)\n\nif (mode == "enter") then\n\nelseif (mode == "process") then\n\n\t-- Superman Returns PC port, Fortress costumes (docs/MODS.md "Fortress costumes").\n\t-- The retail state consumed SetCostumeNum before Superman existed and lost it; wait for him.\n\tlocal supermanID = GetIDFromBinIndex("SUPERMAN", 0)\n\tif (supermanID == -1 or supermanID == 0) then return end\n\n\tlocal costumeNum = GetDASValue("SetCostumeNum")\n\tif (GetDASValue("SRAppliedTo") ~= supermanID) then\n\t\t-- a Superman the game spawned itself (SupermanLayer: SupermanEnt): dress him in the worn suit\n\t\tSetDASValue("SRAppliedTo", supermanID)\n\t\tlocal worn = GetDASValue("SRWornCostume")\n\t\tif (0 == costumeNum and worn > 1) then\n\t\t\tcostumeNum = worn\n\t\tend\n\tend\n\n\tif( 0 ~= costumeNum ) then\n\n\t\t-- Reset for next use\n\t\tSetDASValue("SetCostumeNum", 0)\n\n\t\t-- Costume is Key entry named "Costume#"\n\t\tlocal newCostume = GetDASValue("Costume" .. costumeNum)\n\n\t\t-- only a suit that moves on this session\'s animation banks (the others: next start)\n\t\tif( nil ~= newCostume and GetDASValue("SRCostumeFamily" .. costumeNum) == GetDASValue("SRActiveFamily") ) then\n\t\t\tLuaDebugPrint("Costume switch: " .. newCostume)\n\t\t\tReplaceEntityVisRep(supermanID, newCostume);\n\t\t\tSetDASValue("SRWornCostume", costumeNum)\n\t\tend\n\tend\n\nelseif (mode == "exit") then\n\nend\n\nend\n'
WORN_REQUEST_LUA = b'local srSuit = "SupermanEnt"\n\tif ( IsWorldFlowRunning("SupermenActorWorldFlow") ) then\n\t\tlocal srWorn = GetWorldFlowDASValue("SupermenActorWorldFlow", "SRWornCostume")\n\t\tif (srWorn ~= nil and srWorn > 1) then\n\t\t\tlocal srWornSuit = GetWorldFlowDASValue("SupermenActorWorldFlow", "Costume" .. srWorn)\n\t\t\tif (srWornSuit ~= nil) then srSuit = srWornSuit end\n\t\tend\n\tend\n\tReplaceEntityVisRep(supermanID, srSuit);'


def compose_switch_lua(base: bytes, entry: str) -> bytes:
    """Match modComposeCostumeSwitchLua; never schedules a guest call."""
    if not base or len(base) > 1024 * 1024:
        raise ValueError("invalid costume Lua input")
    text = base[:-1] if base.endswith(b"\0") else base
    if b"\0" in text or not text.startswith(b"function main(mode)"):
        raise ValueError("costume switch needs retail Lua source")
    if entry == "SUPERMENACTORWORLDFLOWFSM_S_60265281":
        if any(text.count(stmt) != 1 for stmt in RETAIL_COSTUME_STATE):
            raise ValueError("costume actor Lua statement missing or ambiguous")
        return COSTUME_SWITCH_LUA
    if entry == "BASEWORLDFLOWFSM_S_7E412353":
        marker = b'SetWorldFlowDASValue("SupermenActorWorldFlow", "SetCostumeNum", 1)'
        replacement = b"-- PC port: the actor flow re-dresses the respawned Superman in the worn suit"
    elif entry == "BASEWORLDFLOWFSM_S_4982930A":
        marker = b'ReplaceEntityVisRep(supermanID, "SupermanEnt");'
        replacement = WORN_REQUEST_LUA
    else:
        raise ValueError("unsupported costume Lua entry")
    if text.count(marker) != 1:
        raise ValueError("costume Lua statement missing or ambiguous")
    return text.replace(marker, replacement)


def valid_superman_bank(b):
    """kernel/mod_costume_data.cpp modValidSupermanBank."""
    return (16 <= len(b) <= 4 * 1024 * 1024 and struct.unpack_from(">I", b, 4)[0] == 9 and
            struct.unpack_from(">I", b, 12)[0] == 0xE3D7527A and struct.unpack_from(">I", b, 0)[0] < len(b))


def _bgfa_find(b, base, n, entry):
    if n < 64 or b[base:base + 8] != b"BGFA1.05":
        raise ValueError(entry + ": container is not BGFA1.05")
    count, toc = struct.unpack_from("<II", b, base + 12)
    toc_len = struct.unpack_from("<I", b, base + 24)[0]
    tags, names = struct.unpack_from("<II", b, base + 40)
    wa, wi, wo, ws, wu, shift = b[base + 33:base + 39]
    record = 1 + wa + wi + wo + ws + wu + names
    if toc > n or toc_len > n - toc or 4 * tags > toc_len or count > (toc_len - 4 * tags) // record:
        raise ValueError(entry + ": BGFA TOC out of range")
    key = srvfs.fnv1a64_upper(entry) & ((1 << (8 * wi)) - 1)
    for i in range(count):
        at = base + toc + tags * 4 + i * record + 1 + wa
        if int.from_bytes(b[at:at + wi], "little") != key:
            continue
        off = int.from_bytes(b[at + wi:at + wi + wo], "little") << shift
        size_at = at + wi + wo
        size = int.from_bytes(b[size_at:size_at + ws], "little")
        unc = int.from_bytes(b[size_at + ws:size_at + ws + wu], "little") if wu else 0
        if off > n or size > n - off:
            raise ValueError(entry + ": BGFA payload out of range")
        return base + off, size, size_at, ws, unc != 0
    raise ValueError(entry + ": entry missing")


def patch_baseworld_banks(data, banks):
    """kernel/mod_costume_data.cpp modPatchBaseworldBanks: own banks into their retail slots of
    BASEWORLDFLOW_ANIM, in place (TOC size updated, rest of the slot zero), all-or-nothing."""
    out = bytearray(data)
    anim_off, anim_size, _, _, compressed = _bgfa_find(out, 0, len(out), "BASEWORLDFLOW_ANIM")
    if compressed:
        raise ValueError("BASEWORLDFLOW_ANIM is compressed")
    slots, seen = [], set()
    for name, blob in banks:
        up = name.upper()
        if up not in SUPERMAN_BANKS or up in seen:
            raise ValueError(name + " is not a SupermanAnims bank (or is listed twice)")
        seen.add(up)
        if not valid_superman_bank(blob):
            raise ValueError(name + ": not a Superman-skeleton animation bank")
        off, size, size_at, ws, comp = _bgfa_find(out, anim_off, anim_size, up)
        if comp or not valid_superman_bank(bytes(out[off:off + size])):
            raise ValueError(up + ": the retail slot is not a plain Superman bank")
        if len(blob) > size:
            raise ValueError("%s: own bank %d bytes > retail slot %d" % (up, len(blob), size))
        slots.append((off, size, size_at, ws, blob))
    for off, size, size_at, ws, blob in slots:
        out[off:off + size] = blob + bytes(size - len(blob))
        out[size_at:size_at + ws] = len(blob).to_bytes(ws, "little")
    return bytes(out)
