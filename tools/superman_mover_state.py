#!/usr/bin/env python3
"""superman_mover_state.py - decode every game-side gate that decides whether Superman's
movement is collision-checked, from a Xenia golden dump or one of our minidumps.
Read-only; same image layer as tools/havok_snapshot.py (xenia:<name|path>, ours:<dmp>).

Contracts (IDA, iter-463 collision map; ENGINE_MAP "Superman mover"):
  Movement path, every locomotion type: the entity's loco slot (actor+0x60) component
  (LOCO 0x8200EDF4 / FLY 0x8200ED88 / ANIM 0x8200ED28 / ...) computes a velocity and writes it
  through the PMSY owner (vt 0x82008958, vt+40 -> PhysCharProxyObj vt+60 setVelocity); entity
  phase 2 (sub_821D4CA0 case 2) then runs PhysCharProxyObj vt+8 = MoveTowardIdealPos_sub_821C0C58,
  which calls hkCharacterProxy::integrate 0x825EC5A8 unless one of the gates below says no.
  PhysCharProxyObj ('PRXY', vt 0x82009018, 144 B, ctor sub_821C59D8):
    +0x04 hkCharacterProxy*        +0x0E lo nibble = collision layer, hi nibble = physrep motion
    +0x0F bit0 gravity, bit1 physrep enabled, bit2 ACTIVE (MoveTowardIdealPos returns at once
          when clear)              mode (0 dynamic, 1 KEYFRAMED -> integrate result discarded,
    +0x40 owning actor              2 fixed; set by vt+108 sub_821AE1C8 unless +0x8C locks it)
    +0x64 time in current support state      +0x68 ground-snap height (<0 enables the snap ray)
    +0x7C supported (sub_821B5330: checkSupport == SUPPORTED, or keyframed)
    +0x7D..+0x81 contact flags      +0x83 "no gravity mover" (FLY/ANIM set 1, LOCO 0)
    +0x84 reduced physics (Lua SetReducedPhysicsProcessing, pedestrians)
    +0x88 1 = last tick took the OFF-CAMERA SKIP (pos += v*dt, no integrate)
    +0x8C keyframed lock (def+2412)
  Off-camera skip (0x821C0CDC..0x821C0DD0): +0x7C && +0x84 == 1 && (|pos - cam|^2 > 900 ||
  !NDC-visible sub_82175378).  Layer: phantom filterInfo low 5 bits; a layer whose row in the
  GameGroupFilter table (world+0x7C, +0x1C) lacks bit 7 ignores buildings (11 = physrep disabled).

Usage:
  python tools/superman_mover_state.py xenia:sound_gameplay
  python tools/superman_mover_state.py ours:archive/v1_invest/collision/ours_midtown.dmp [--json]
      [--actor 0x841C9ED0]    (skip the whole-memory scan; actor from havok_snapshot's output)
Also prints the proxy phantom's overlap list (+0x120/+0x124): the only collidables the
linear cast tests; a building whose AABB overlaps the swept capsule but is absent here is
invisible to integrate.
"""
from __future__ import annotations

import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import havok_snapshot as hs  # noqa: E402

VT_PRXY_OBJ = 0x82009018
VT_ACTOR = 0x82009798
VT_PMSY = 0x82008958
LOCO_VT = {0x8200EDF4: "LOCO", 0x8200ED88: "FLY", 0x8200ED28: "ANIM", 0x8200EDBC: "GRND",
           0x8200ED58: "CPTR", 0x8200F030: "WHLD", 0x82008D50: "NONE"}
FLY_SUBMOVE = {0: "boost", 1: "brake", 2: "hover", 3: "land", 4: "default"}
G_MOVEDEFS = 0x8296DF58      # {base, cap, count} of move definitions; def+4 = name Str
G_PHYS = 0x8296F244          # phys manager; +12 = hkWorld
MODE = {0: "dynamic", 1: "KEYFRAMED (integrate discarded)", 2: "fixed", 4: "mode4"}


def f32(img, va):
    b = img.read(va, 4)
    return struct.unpack(">f", b)[0] if len(b) == 4 else None


def movedef_name(img, idx):
    tab = img.u32(G_MOVEDEFS)
    if not tab or idx is None or idx >= 0x10000:
        return None
    base, count = img.u32(tab), img.u32(tab + 8)
    if base is None or count is None or idx >= count:
        return None
    d = img.u32(base + 4 * idx)
    for k in range(4):
        p = img.u32(d + 4 + 4 * k) if d else None
        if p and 0x10000 < p < 0xA0000000:
            s = img.cstr(p, 64)
            if s and len(s) > 2 and s.isprintable():
                return s
    return None


def filter_row(img, layer):
    w = img.u32(img.u32(G_PHYS) + 12) if img.u32(G_PHYS) else None
    f = img.u32(w + 0x7C) if w else None
    return img.u32(f + 0x1C + 4 * layer) if f else None


def find_superman(img, actor=None):
    if actor is not None:                     # skip the (slow on our dumps) memory scan
        handler = img.u32(actor + 0x84)
        name = img.cstr(img.u32(handler + 0x24) or 0) if handler else None
        return actor, img.u32(actor + 0x70), name
    rdata = (0x82000600, 0x820BDA3C)
    hits = img.scan_u32([VT_PRXY_OBJ, VT_ACTOR], 0, 0x100000000)
    live = lambda vt: [a for a in hits[vt] if not (rdata[0] <= a < rdata[1])]
    prxy = set(live(VT_PRXY_OBJ))
    for a in live(VT_ACTOR):
        phys, handler = img.u32(a + 0x70), img.u32(a + 0x84)
        name = img.cstr(img.u32(handler + 0x24) or 0) if handler else None
        if phys in prxy and name and name.upper().startswith("SUPERMAN"):
            return a, phys, name
    return None, None, None


def overlaps(img, phantom):
    """hkSimpleShapePhantom overlap list (+0x120 array, +0x124 count): the ONLY collidables
    setPositionAndLinearCast (sub_8259EC40) tests; maintained by hkPhantom_updateBroadPhase."""
    arr, n = img.u32(phantom + 0x120), img.u32(phantom + 0x124)
    if not arr or n is None or n > 4096:
        return None
    out = []
    for i in range(n):
        c = img.u32(arr + 4 * i)
        body = c - 0x14 if c else None          # hkEntity collidable at +0x14 (havok_snapshot E_COLLIDABLE)
        out.append({"collidable": "0x%08X" % (c or 0), "owner": "0x%08X" % (body or 0),
                    "owner_filterInfo": "0x%08X" % (img.u32(c + 0x1C) or 0) if c else None})
    return out


def decode(img, actor=None):
    actor, po, fsm = find_superman(img, actor)
    if not actor:
        return {"error": "Superman actor (vt 0x82009798, FSM SUPERMAN*) with a PRXY physics child not found"}
    b14, b15 = img.u8(po + 0x0E), img.u8(po + 0x0F)
    proxy = img.u32(po + 0x04)
    phantom = img.u32(proxy + hs.CP_PHANTOM) if proxy else None
    fi = img.u32(phantom + hs.P_FILTER_INFO) if phantom else None
    layer = fi & 0x1F if fi is not None else None
    row = filter_row(img, layer) if layer is not None else None
    reduced = img.u32(po + 0x84)
    supported = img.u8(po + 0x7C)
    slot = img.u32(actor + 0x60)
    comp = img.u32(slot + 4) if slot else None
    cvt = img.u32(comp) if comp else None
    ctype = LOCO_VT.get(cvt, "0x%08X" % cvt if cvt else None)
    mdi = img.u32(comp + 0x60) if comp else None
    sub = img.u32(comp + 0x90) if comp else None
    out = {
        "actor": "0x%08X" % actor, "fsm": fsm, "pos": [round(x, 3) for x in img.vec(actor + 0x20, 3)],
        "prxy_obj": "0x%08X" % po, "hkCharacterProxy": "0x%08X" % (proxy or 0),
        "b0E": "0x%02X" % b14, "layer_nibble": b14 & 0xF, "physrep_mode": MODE.get(b14 >> 4, b14 >> 4),
        "b0F": "0x%02X" % b15, "gravity": bool(b15 & 1), "physrep_enabled": bool(b15 & 2),
        "ACTIVE_mover_runs": bool(b15 & 4),
        "supported_7C": supported, "support_timer_64": f32(img, po + 0x64),
        "snap_height_68": f32(img, po + 0x68),
        "contact_flags_7D_81": [img.u8(po + k) for k in range(0x7D, 0x82)],
        "no_gravity_mover_83": img.u8(po + 0x83),
        "reduced_physics_84": reduced, "last_tick_skipped_88": img.u32(po + 0x88),
        "keyframed_lock_8C": img.u8(po + 0x8C),
        "phantom_filterInfo": "0x%08X" % fi if fi is not None else None, "layer": layer,
        "layer_collides_with_buildings_L7": bool(row >> 7 & 1) if row is not None else None,
        "layer_collides_with_terrain_L6": bool(row >> 6 & 1) if row is not None else None,
        "loco_slot": "0x%08X" % (slot or 0), "loco_slot_flag_8": img.u8(slot + 8) if slot else None,
        "loco_owner_vt_ok": (img.u32(img.u32(slot) or 0) == VT_PMSY) if slot else None,
        "loco_type": ctype, "movedef": mdi, "movedef_name": movedef_name(img, mdi),
        "loco_submove_90": (FLY_SUBMOVE.get(sub, sub) if ctype == "FLY" else sub),
    }
    ov = overlaps(img, phantom) if phantom else None
    out["phantom_overlaps_n"] = len(ov) if ov is not None else None
    out["phantom_overlaps"] = ov
    gates = []
    if not out["ACTIVE_mover_runs"]:
        gates.append("+0x0F bit2 clear: MoveTowardIdealPos does nothing (no integrate, no move)")
    if (b14 >> 4) == 1:
        gates.append("KEYFRAMED physrep: integrate result discarded, pos += v*dt (no collision)")
    if supported and reduced == 1:
        gates.append("off-camera skip ARMED (+0x7C && +0x84==1): skips integrate when >30 m or off-screen")
    if out["layer_collides_with_buildings_L7"] is False:
        gates.append("proxy layer %s does not collide with layer 7 buildings" % layer)
    if out["loco_slot_flag_8"]:
        gates.append("loco slot flag +8 set: component update replaced by adapter vt+64")
    out["gates_open"] = gates or ["none: movement is velocity -> hkCharacterProxy::integrate"]
    return out


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 1
    img = hs.open_image(argv[0])
    actor = int(argv[argv.index("--actor") + 1], 16) if "--actor" in argv else None
    out = decode(img, actor)
    out["image"] = argv[0]
    if "--json" in argv:
        print(json.dumps(out, indent=1))
    else:
        for k, v in out.items():
            print("%-34s %s" % (k, v))
    return 0 if "error" not in out else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
