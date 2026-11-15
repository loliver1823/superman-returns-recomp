#!/usr/bin/env python3
"""make_charswitch_test_pack.py - a test character pack whose characters each run on their OWN
animation project: scoped copies of the retail SupermanAnims (docs/MODS.md "Character packs").

Everything is derived from the player's own disc at build time (nothing from the disc is committed):
  - <P><C>CCT.rcb       byte-identical copy of BASEWORLDFLOW_ANIM/SUPERMANCCTPROJECT
  - <P><C><SET>.bnk     the six retail SUPERMAN_* banks with new identities only: header +08 =
                        FNV-1(bank name), every clip block's +00 frame-cache hash = FNV-1(<P><C> + old)
                        (bone data, clip names, events and timing are unchanged)
  - <P><C>ANIMS.xml     <project name="<P><C>ANIMS" rcbfile="<P><C>CCT"> + the six banks in retail order
  - <P><C>ENT.xml       a retail Superman entity (SupermanEnt / SupermanClassicEnt / SupermanGrayEnt look)
                        with only animsys project changed to <P><C>ANIMS
so a switch to one of these characters provably changes the actor's project (AnimSys +0x64 =
FNV-1(<P><C>ANIMS)) while it must look and move exactly like retail Superman in that suit.

The pack is "test_only": its suits appear only with SR_TEST_CHARACTERS=1 (never in a player's picker).
Usage: python tools/srmods/make_charswitch_test_pack.py --disc "Superman Returns (USA)" --out mods/charswitch_test
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import srmods  # noqa: E402

MOD_ID = "charswitch_test"
SETS = [("FLIGHT", "Flight"), ("ONFOOT", "OnFoot"), ("MELEEGROUND", "MeleeGround"),
        ("SUPER_POWERS", "Super_Powers"), ("SUPERSPEED_ATTACKS", "Superspeed_Attacks"), ("MELEEAIR", "MeleeAir")]
# character id -> (screen name, retail look: visrep line, clothanimdef or "", description)
LOOKS = {
    "CLASSIC": ("TEST: GOLDEN AGE (OWN ANIMS)",
                '<visrep type="SkinnedModelCloth" asset="X2_Superman_Alt" clothdef="SupermanClassicClothCape" renderbin="Superman"/>',
                ' clothanimdef="SupermanCape"',
                "Charswitch test character: the Golden Age suit on its own copy of Superman's animation project."),
    "POD": ("TEST: POD SUIT (OWN ANIMS)",
            '<visrep type="SkinnedModel" asset="X2_supermanGray" renderbin="Superman"/>',
            "",
            "Charswitch test character: the Pod suit on its own copy of Superman's animation project."),
}
ENTITY = """<entity type="Actor" scale="1" filter="SUPERMAN|CAMERATARGET|TRAFFICOBSTACLE">
  {visrep}
  <audiorep type="ACTOR" material="SND_CORE_COMBAT_BODY_HIT">
    <audioproperty type="SND_FLIGHT_ENGINE"/>
    <audioproperty type="SND_AMBIENCE_NORMAL"/>
    <audioproperty type="SND_MUSIC"/>
  </audiorep>
  <physrep type="CHARPROXY" vertexa="0 0.4 0" vertexb="0 -0.83 0" radius="0.3" mass="200" maxslope="60"/>
  <animsys project="{project}" statemgr="" jointaimer="SupermanJointAimer"{clothanim}/>
  <attribsys asset="SupermanAttributes"/>
  <fsm asset="SupermanFSM_g_21A4B50D"/>
  <linkagesys type="Superman" offset="0 0 0" rotation="0 0 0"/>
  <movesys type="Superman"/>
  <powersys asset="SupermanPowers"/>
  <targetsys asset="SupermanTargetSys"/>
</entity>
"""


def fnv1_32(s):
    h = 0x811C9DC5
    for c in s.encode("latin1"):
        h = (h * 0x01000193) & 0xFFFFFFFF
        h ^= c
    return h


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def scoped_bank(src, bank_name, namespace, used):
    """Retail bank -> same bytes with a new bank identity and new frame-cache block hashes."""
    b = bytearray(src)
    if u32(b, 4) != 9 or u32(b, 12) != 0xE3D7527A:
        raise ValueError("%s: not a Superman-skeleton bank" % bank_name)
    struct.pack_into(">I", b, 8, fnv1_32(bank_name))
    nd, pd = u32(b, 0x2C), u32(b, 0x30)
    if nd > 100000 or pd + 4 * nd > len(b):
        raise ValueError("%s: implausible block table" % bank_name)
    for i in range(nd):
        off = u32(b, pd + 4 * i)
        if off + 8 > len(b) or u32(b, off + 4) != 8:
            raise ValueError("%s: block %d is not a clip block" % (bank_name, i))
        old = u32(b, off)
        h, k = fnv1_32("%s_%08x" % (namespace, old)), 0
        while h in used:
            k += 1
            h = fnv1_32("%s_%08x_%d" % (namespace, old, k))
        used.add(h)
        struct.pack_into(">I", b, off, h)
    return bytes(b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disc", required=True, help="the game folder (contains DATA/baseworl.AST)")
    ap.add_argument("--out", required=True, help="mods/<pack> folder to (re)write")
    ap.add_argument("--chars", default="CLASSIC,POD")
    a = ap.parse_args()
    anim = srmods.read_top_entry(os.path.join(a.disc, "DATA", "baseworl.AST"), "BASEWORLDFLOW_ANIM")
    import srvfs
    entries = {}
    for e in srvfs.Bgfa(srvfs.Src.mem(anim)).entries:
        entries[str(getattr(e, "name", "") or "").upper()] = e.data.read(0, e.size)
    prefix = srmods.asset_prefix(MOD_ID)
    loose = os.path.join(a.out, "DATA", "_workinprogress", MOD_ID)
    os.makedirs(loose, exist_ok=True)
    costumes, used = [], set()
    for cid in [c for c in a.chars.split(",") if c]:
        name, visrep, clothanim, desc = LOOKS[cid]
        P = prefix + cid + "_"
        project = P + "ANIMS"
        banks = []
        for setname, _ in SETS:
            bank = P + setname
            data = scoped_bank(entries["SUPERMAN_" + setname], bank, P, used)
            with open(os.path.join(loose, bank + ".bnk"), "wb") as f:
                f.write(data)
            banks.append(bank)
        with open(os.path.join(loose, P + "CCT.rcb"), "wb") as f:
            f.write(entries["SUPERMANCCTPROJECT"])
        xml = '<project name="%s" rcbfile="%sCCT">\n' % (project, P)
        xml += "".join('  <bank name="%s"/>\n' % bk for bk in banks) + "</project>\n"
        with open(os.path.join(loose, project + ".xml"), "w", newline="\n") as f:
            f.write(xml)
        with open(os.path.join(loose, P + "ENT.xml"), "w", newline="\n") as f:
            f.write(ENTITY.format(visrep=visrep, project=project, clothanim=clothanim))
        costumes.append({"id": cid.lower(), "name": name, "entity": P + "ENT",
                         "animation_project": project, "description": desc})
        print("%s: project %s hash %08x" % (cid, project, fnv1_32(project)))
    manifest = {"format": 1, "id": MOD_ID, "name": "Charswitch test characters",
                "author": "Superman Returns PC port (generated from the player's own disc)",
                "version": "0.1.0", "category": "characters", "game": "SRX360-USA", "test_only": True,
                "description": "Test characters for the character switch: retail looks on their own "
                               "copies of Superman's animation project. Diagnostic content.",
                "costumes": costumes}
    with open(os.path.join(a.out, "mod.json"), "w", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    main()
