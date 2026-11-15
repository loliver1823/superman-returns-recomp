#!/usr/bin/env python3
"""havok_snapshot.py - read-only census of the Havok 3.2.0 world in a memory image.

WHY: the building-collision investigation (iter-463, archive/v1_invest/collision/)
ends in one decisive test: snapshot the Havok state in OUR build and in Xenia at the
same place and diff them as SETS. Both sides must go through IDENTICAL reading code
(memory/feedback_tool_verification_protocol.md), so this one reader serves both:

  xenia:<dump>   a Xenia minidump (data/xenia_canary*.DMP, or a golden name:
                 nosound | sound_menu | sound_gameplay). Guest VA -> host
                 0x100000000 + VA (+0x1000 for VA >= 0xE0000000), via tools/golden.py.
  snap:<file>    a .snap of just the guest pages a reader touched (tools/guest_snap.py,
                 written by `objstate.py snap --write-snap`); reads outside it come back short.
  ours:<dump>    a full minidump of SupermanReturns.exe (cdb .dump /ma). Guest VA ->
                 base + VA (flat). The base is read from the exported g_debugGuestBase
                 (kernel/guest_context.cpp) through the dump's module list + PE export
                 table; fallback = the harness's preferred base 0x2A00000000
                 (guest/guest_harness.cpp allocGuestMemory), or --base. Every base is
                 SELF-CHECKED (image MZ at 0x82000000, hkClass registry string, hkWorld
                 vtable) - the startup mirror at base-4GB passes MZ but fails the world check.

Usage:
  python tools/havok_snapshot.py snap --image xenia:sound_gameplay [--label NAME]
  python tools/havok_snapshot.py snap --image ours:archive/.../ours.dmp [--base 0x2A00000000]
         [--near=x,y,z] [--radius R]      # default near = Superman's proxy position
  python tools/havok_snapshot.py diff A.json B.json [--near=x,y,z | --near=a | --near=b] [--radius R]
  (write --near=... with '=': a negative x would otherwise parse as an option; a/b = that
  snapshot's Superman position; no --near = every body)

Outputs (default dir archive/v1_invest/collision/snapshots/, gitignored):
  <label>.json (full) and <label>.txt (summary); diff prints and writes <A>_vs_<B>.txt.

Every offset below cites the decompiled function that reads or writes it (IDA,
retail default.xex). Offsets marked [data] were derived from golden memory and are
guarded by a structural self-check the tool runs on every image.
Read-only: nothing here writes guest memory or calls guest code.
"""
from __future__ import annotations

import argparse
import json
import math
import mmap
import os
import re
import struct
import sys
import zlib
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_OUT = os.path.join(REPO, "archive", "v1_invest", "collision", "snapshots")
SCHEMA = "havok_snapshot/1"

# ---------------------------------------------------------------- guest layout ----
# Globals
G_PHYS_MGR = 0x8296F244      # physMgr ptr; hkWorld = *(physMgr+12): PhysWorld_AddPhantomIfNotInWorld_sub_822BE710
                             # (result+12), PhysCharProxy_Create_sub_821B9A18 (*(dword_8296F244+12)+32 = gravity)
G_HK_MEMORY = 0x82979460     # hkMemory* ; +4 = memory-failure flag set to 1 by hkSimulation step sub_825D2B38,
                             # +8 = soft limit, +0x14/+0x28 = used (hkWorld_stepDeltaTime_sub_82595F00)
G_LOOP_MGR = 0x8296D8E0      # game kernel loop mgr: +0x34 step mode, +0x48 period ns (u64), +0x84 dt float
                             # (GameKernelLoop_AdvanceTickTime_sub_82720740; read by FeMasterTick_821494C0)
# Image anchors used by the self-check
IMAGE_BASE_VA = 0x82000000
REGISTRY_MOPP_NAME_SLOT = 0x829289E8   # hkClass/vtable registry triple (name*, fn, vt) for hkMoppBvTreeShape

# hkWorld (vt 0x8206D35C written by hkWorld_Ctor_sub_82597D48)
VT_HKWORLD = 0x8206D35C
W_SIMULATION = 0x08          # m_simulation: hkWorld_stepDeltaTime_sub_82595F00 (a1+8 ->step)
W_GRAVITY = 0x20             # PhysCharProxy_Create_sub_821B9A18 (world+32 vector)
W_FIXED_ISLAND = 0x30        # hkWorldOperationUtil_addEntitySI_sub_825AE448 (result+48)
W_FIXED_BODY = 0x34          # [data] the world's own fixed rigid body (entity 0 of the fixed island)
W_ACTIVE_ISLANDS = 0x38      # hkArray {ptr,size,cap}: addEntitySI (v4+14 -> +56)
W_INACTIVE_ISLANDS = 0x44    # hkArray: addEntitySI (v4+17 -> +68)
W_WATCHDOG = 0x60            # m_memoryWatchDog: hkWorld_stepDeltaTime_sub_82595F00 (a1+96)
W_BROADPHASE = 0x64          # hkWorldOperationUtil_addEntityBP_sub_825AFAC0 (v3[25])
W_FILTER = 0x7C              # m_collisionFilter: hkWorld setCollisionFilter sub_8259B650 (a1+124), addEntityBP (v3[31])
W_OP_QUEUE = 0x84            # hkWorldOperationQueue*: addEntity queues op 1 on (a1+132) while locked
W_PENDING_OPS = 0x88         # pending-operation count: hkWorld_addEntity_sub_82596508 (a1+136)
W_CRIT_LOCK = 0x8C           # critical-operations lock count: hkWorld_addEntity_sub_82596508 (a1+140)
W_CRIT_LOCK_PHANTOM = 0x90   # second lock count: hkWorld_addPhantom_sub_82596A60 queues when (a1+144)+(a1+140) != 0
W_BLOCK_PENDING = 0x94       # byte: pending ops run on unlock only if 0 (addEntity/addPhantom: *(char*)(a1+148))
# (a stuck lock count or block byte leaves every later addEntity QUEUED, not in the world: H-A)
W_PHANTOMS = 0xCC            # hkArray<hkPhantom*>: hkWorld::addPhantom sub_82596A60 (a1+204/+208)

# hkSimulation
SIM_LAST_DT = 0x08           # sub_825D2B38 stores the step dt (a1+8)
SIM_PREV_RESULT = 0x10       # sub_825D2B38 (a1+16)
SIM_STATUS = 0x14            # sub_825D2B38 (a1+20; ==1 -> resume collide)

# hkSimulationIsland (vt 0x820710E4, 116 bytes: HkSimIsland_Ctor_sub_825B6440)
VT_ISLAND = 0x820710E4
I_WORLD = 0x1C               # HkSimIsland_Ctor_sub_825B6440 (result+28 = world)
I_STORAGE_INDEX = 0x20       # addEntitySI (u16 +32)
I_ACTIVE = 0x28              # addEntitySI (byte +40: 1 active / 0 inactive)
I_ENTITIES = 0x3C            # hkArray<hkEntity*>: HkSimIsland_InternalAddEntity_sub_825B5B70 (a1+15 words)

# hkEntity / hkRigidBody (vt 0x8206D65C from the hkClass registry)
E_WORLD = 0x08               # addEntitySI (a2+8 = world)
E_USERDATA = 0x0C            # hkWorldObject m_userData [data; 0 on every golden body]
E_NAME = 0x10                # char*: PhysRes_CloneRigidBodyByName_sub_822DED20 (strcmp(name, v6[4]))
E_COLLIDABLE = 0x14          # collidable: addEntityBP (v2 = a2+20)
E_SHAPE = 0x14               #   collidable.m_shape: addEntityBP (*v2 -> shape vt+12 getAabb)
E_TRANSFORM = 0x1C           #   collidable.m_motion (hkTransform*): addEntity (a2[7]), addEntityBP (v2[2])
E_BP_HANDLE = 0x28           #   broadphase handle (collidable+0x14): addEntityBP (v2+5)
E_BP_ID = 0x28               #     handle.m_id (index into hk3AxisSweep nodes) [data, backref-checked]
E_BP_TYPE = 0x2C             #     handle.m_type (byte) [data]
E_FILTER_INFO = 0x30         #   collidable+0x1C: sub_82578CA0 passes *(collidable+28) to isCollisionEnabled
E_MOTION = 0x50              # hkMotion*: HkSimIsland_InternalAddEntity_sub_825B5B70 (a2+80 ->vt[7])
E_ISLAND = 0x54              # m_simulationIsland: HkSimIsland_InternalAddEntity (a2+84)
E_ISLAND_INDEX = 0x8C        # u16 storage index: HkSimIsland_InternalAddEntity (a2+140)
E_FIXED = 0x91               # byte: addEntitySI routes !=0 to the fixed island (a2+145)
T_TRANSLATION = 0x30         # hkTransform {hkRotation 3 x hkVector4, hkVector4 translation} (Havok layout;
                             # guarded by an orthonormality check on the rotation columns)

# Shapes
VT_MOPP = 0x8206BDBC
VT_LIST = 0x8206BE80
VT_MOPP_CODE = 0x82071618
MOPP_CHILD = 0x0C            # "Mesh" child shape collection: sub_82577B50 (a1+12); castRay sub_82577C78 (a2+12)
MOPP_CODE = 0x10             # hkMoppCode*: sub_82577B50 (a1+16), dtor sub_825779E0 (a1[4] released)
MC_OFFSET = 0x10             # hkMoppCode m_info.m_offset vec4 [data]
MC_DATA = 0x20               # hkMoppCode byte array ptr: dtor sub_825BB4E0 (result[8])
MC_SIZE = 0x24               # size: sub_82577B50 (*(code+36))
MC_CAP = 0x28                # capacity|flags: dtor sub_825BB4E0 (result[10])
LIST_CHILDREN = 0x10         # hkListShape child array (8-byte childInfo, shape at +0): sub_82578830 (a1[4])
LIST_COUNT = 0x14            # sub_82578830 (a1[5])

# Broadphase hk3AxisSweep (vt 0x82072068; dtor sub_825D97C0)
VT_3AXIS = 0x82072068
BP_OFFSET_LOW = 0x10         # [data] vec3 (25000 x3 in golden), quantized = (x + offLow) * scale
BP_SCALE = 0x30              # [data] vec3
BP_NODES = 0x40              # hkArray<hkBpNode 16B>: dtor sub_825D97C0 (a1[16], 16*cap)
BP_AXES = (0x4C, 0x58, 0x64)  # 3 x hkArray<hkBpEndPoint 4B {u16 value,u16 node}>: dtor sub_825D97C0 (a1+28 -3*i)
# hkBpNode: u16 min_y,min_z,max_y,max_z,min_x,max_x; +12 handle* [data, every index backref-checked]

# Collision filter (game class, vt 0x8201F7D4, ctor sub_822C49F8; base hkGroupFilter ctor sub_82578BA8)
VT_GAME_FILTER = 0x8201F7D4
F_NEXT_FREE_GROUP = 0x18     # sub_82578BA8 (a1[6] = 0)
F_TABLE = 0x1C               # 32 words: sub_82578BA8 (a1+7), isCollisionEnabled sub_82578A68 (4*(layer+7))

# Phantoms
P_SHAPE = 0x14               # same hkWorldObject collidable layout as entities
P_TRANSFORM = 0x1C           # hkWorld::addPhantom sub_82596A60 (a2[7])
P_FILTER_INFO = 0x30

# Character proxy (hkCharacterProxy, 176 B, vt triple at +0/+8/+0xC: dtor sub_825EC390)
VT_CHAR_PROXY = 0x82072C94
VT_CHAR_PROXY_2 = 0x82072C80
VT_CHAR_PROXY_3 = 0x82072C6C
CP_PHANTOM = 0x30            # m_shapePhantom: dtor sub_825EC390 (a1[12])
CP_MANIFOLD = 0x74           # hkArray<hkRootCdPoint 48B>: dtor (a1[29], 48*cap)
CP_LISTENERS = 0x80          # hkArray: dtor (a1[32])
CP_BODIES = 0x8C             # hkArray<hkRigidBody*> it listens to: dtor (a1[35], removeEntityListener sub_8259DA38)
CP_PHANTOMS = 0x98           # hkArray<hkPhantom*>: dtor (a1[38], removePhantomListener sub_8259F7C8)
RCP_COLL_A = 0x20            # hkRootCdPoint m_rootCollidableA [data: == phantom+0x14 in golden]
RCP_COLL_B = 0x28            # m_rootCollidableB [data: == entity+0x14]
# Game side: PhysCharProxyObj ('PRXY', vt 0x82009018, PhysCharProxyObj_Ctor_sub_821C59D8) +4 = hkCharacterProxy
# (PhysCharProxy_Create_sub_821B9A18: a1+4 = proxy); the owning actor (vt 0x82009798,
# ScreenActorCtor8_sub_821D0DD0) holds it in child slot +0x70 (RegistryEntrySetChildSlot112) and its
# FSM handler in slot +0x84 (ScreenEntrySetHandlerSlot132); handler+0x24 = FSM name [data]
# ("SUPERMANFSM_..." / "CITIZENFSM_..." in golden sound_gameplay).
VT_PRXY_OBJ = 0x82009018
PRXY_PROXY = 0x04
VT_ACTOR = 0x82009798
ACTOR_PHYS_CHILD = 0x70
ACTOR_HANDLER = 0x84
HANDLER_NAME = 0x24

KNOWN_VT = {
    VT_HKWORLD: "hkWorld", VT_ISLAND: "hkSimulationIsland", VT_GAME_FILTER: "GameGroupFilter",
    VT_CHAR_PROXY: "hkCharacterProxy", VT_PRXY_OBJ: "PhysCharProxyObj", VT_ACTOR: "GameActor",
    VT_3AXIS: "hk3AxisSweep",
}
WORLD_LIMIT = 25000.0        # broadphase border (PhysBroadPhaseBorder_MaxPositionExceeded_sub_822C6870, +-25000)
MAX_ARRAY = 200000

FALLBACK_SECTIONS = {".rdata": (0x82000600, 0x820BDA3C), ".data": (0x82910000, 0x829B2FB8)}


# ======================================================================= images ====
class Image:
    """Guest-memory view. Subclasses implement read(va, n) and iter_chunks(lo, hi)."""
    kind = "abstract"

    def read(self, va, n):  # pragma: no cover - interface
        raise NotImplementedError

    def iter_chunks(self, lo, hi):  # yields (guest_va, bytes-like) covering present memory
        raise NotImplementedError

    def u8(self, va):
        b = self.read(va, 1)
        return b[0] if len(b) == 1 else None

    def u16(self, va):
        b = self.read(va, 2)
        return struct.unpack(">H", b)[0] if len(b) == 2 else None

    def u32(self, va):
        if va is None:
            return None
        b = self.read(va & 0xFFFFFFFF, 4)
        return struct.unpack(">I", b)[0] if len(b) == 4 else None

    def f32(self, va):
        b = self.read(va, 4)
        return struct.unpack(">f", b)[0] if len(b) == 4 else None

    def vec(self, va, n=3):
        b = self.read(va, 4 * n)
        return list(struct.unpack(">%df" % n, b)) if len(b) == 4 * n else None

    def cstr(self, va, maxlen=96):
        if not va:
            return None
        b = self.read(va, maxlen)
        if not b:
            return None
        s = b.split(b"\0")[0]
        if not s or not all(32 <= c < 127 for c in s):
            return None
        return s.decode("latin1")

    def scan_u32(self, values, lo=0, hi=0x100000000, limit=1 << 20):
        """{value: [aligned guest VAs whose big-endian u32 == value]} in ONE pass over memory
        (chunks are <= 64 MiB and 4-aligned, so an aligned word never straddles two)."""
        pats = {v: struct.pack(">I", v) for v in values}
        hits = {v: [] for v in values}
        for va0, data in self.iter_chunks(lo, hi):
            for v, pat in pats.items():
                i = data.find(pat)
                while i != -1 and len(hits[v]) < limit:
                    if (va0 + i) & 3 == 0:
                        hits[v].append(va0 + i)
                    i = data.find(pat, i + 1)
        return hits

    def describe(self):
        return {"kind": self.kind}


class SyntheticImage(Image):
    """Sparse guest memory for tests: plant(va, bytes)."""
    kind = "synthetic"

    def __init__(self):
        self.pages = {}

    def plant(self, va, data):
        for i, byte in enumerate(data):
            a = va + i
            pg = self.pages.setdefault(a >> 12, bytearray(4096))
            pg[a & 0xFFF] = byte

    def read(self, va, n):
        out = bytearray()
        for a in range(va, va + n):
            pg = self.pages.get(a >> 12)
            if pg is None:
                break
            out.append(pg[a & 0xFFF])
        return bytes(out)

    def iter_chunks(self, lo, hi):
        for p in sorted(self.pages):
            va = p << 12
            if va + 4096 <= lo or va >= hi:
                continue
            yield va, bytes(self.pages[p])


class _MinidumpImage(Image):
    """Common minidump plumbing (Memory64ListStream + module list)."""

    def __init__(self, path):
        self.path = path
        self._f = open(path, "rb")
        self._m = mmap.mmap(self._f.fileno(), 0, access=mmap.ACCESS_READ)
        sig, _v, nstreams, dir_rva = struct.unpack_from("<4sIII", self._m, 0)
        if sig != b"MDMP":
            raise ValueError("%s: not a minidump" % path)
        self.ranges = []
        self.modules = []
        for i in range(nstreams):
            stype, _dsize, drva = struct.unpack_from("<III", self._m, dir_rva + i * 12)
            if stype == 9:   # Memory64ListStream
                nr, base_rva = struct.unpack_from("<QQ", self._m, drva)
                off = base_rva
                for j in range(nr):
                    start, size = struct.unpack_from("<QQ", self._m, drva + 16 + j * 16)
                    self.ranges.append((start, size, off))
                    off += size
            elif stype == 4:  # ModuleListStream
                (count,) = struct.unpack_from("<I", self._m, drva)
                for j in range(count):
                    mo = drva + 4 + j * 108
                    base, size = struct.unpack_from("<QI", self._m, mo)
                    name_rva = struct.unpack_from("<I", self._m, mo + 20)[0]
                    (nlen,) = struct.unpack_from("<I", self._m, name_rva)
                    name = self._m[name_rva + 4: name_rva + 4 + nlen].decode("utf-16-le", "replace")
                    self.modules.append((name, base, size))
        if not self.ranges:
            raise ValueError("%s: no Memory64ListStream (need a full-memory dump: .dump /ma)" % path)
        self.ranges.sort()

    def read_host(self, addr, size):
        out = bytearray()
        pos, need = addr, size
        for start, rsize, foff in self.ranges:
            if pos >= start + rsize or pos + need <= start:
                continue
            if start > pos:
                break
            take = min(need, start + rsize - pos)
            o = foff + (pos - start)
            out += self._m[o:o + take]
            pos += take
            need -= take
            if need <= 0:
                break
        return bytes(out)

    def _host_windows(self):  # [(guest_lo, guest_hi, host_lo)]
        raise NotImplementedError

    def read(self, va, n):
        for glo, ghi, hlo in self._host_windows():
            if glo <= va < ghi:
                n = min(n, ghi - va)
                return self.read_host(hlo + (va - glo), n)
        return b""

    def iter_chunks(self, lo, hi):
        for glo, ghi, hlo in self._host_windows():
            a, b = max(lo, glo), min(hi, ghi)
            if a >= b:
                continue
            h0, h1 = hlo + (a - glo), hlo + (b - glo)
            for start, rsize, foff in self.ranges:
                s, e = max(start, h0), min(start + rsize, h1)
                step = 64 << 20
                while s < e:
                    t = min(e, s + step)
                    data = self._m[foff + (s - start): foff + (t - start)]
                    if data.count(0) != len(data):   # skip all-zero chunks (most of our 4 GiB)
                        yield glo + (s - hlo), data
                    s = t


class XeniaImage(_MinidumpImage):
    """Xenia canary dump: guest VA X at host 0x100000000 + X (+0x1000 for X >= 0xE0000000; golden.py)."""
    kind = "xenia"

    def __init__(self, name_or_path):
        sys.path.insert(0, HERE)
        import golden  # noqa: E402  (shared dump-name table and E-view constants)
        path = golden.resolve_dump(name_or_path)
        super().__init__(path)
        gb, ev, eo = golden.GUEST_HOST_BASE, golden.E_VIEW_BASE, golden.E_VIEW_HOST_OFFSET
        self._win = [(0, ev, gb), (ev, 0x100000000, gb + ev + eo)]

    def _host_windows(self):
        return self._win

    def describe(self):
        return {"kind": self.kind, "path": os.path.abspath(self.path), "host_base": "0x100000000",
                "e_view_offset": "0x1000"}


class OursImage(_MinidumpImage):
    """Our process dump: guest VA X at base + X (flat allocation, guest/guest_harness.cpp)."""
    kind = "ours"
    PREFERRED_BASE = 0x2A00000000   # allocGuestMemory kPreferredBase
    MIRROR_BASE = 0x2900000000      # startup copy (EDRAM workaround) - must NOT be picked

    def __init__(self, path, base=None):
        super().__init__(path)
        self.base_method = None
        self.base = None
        self.base_candidates = []
        if base is not None:
            self._set(base, "explicit --base")
        else:
            exp = self._export_base()
            if exp:
                self.base_candidates.append(("export g_debugGuestBase", exp))
            self.base_candidates.append(("preferred 0x2A00000000", self.PREFERRED_BASE))
            chosen = None
            for how, b in self.base_candidates:
                self._set(b, how)
                chk = image_self_check(self)
                if chk["image_ok"] and chk["world_ok"]:
                    chosen = (how, b)
                    break
                if chk["image_ok"] and chosen is None:
                    chosen = (how, b)
            if chosen is None:
                raise ValueError("no guest base passes the image self-check; pass --base")
            self._set(chosen[1], chosen[0])

    def _set(self, base, how):
        self.base, self.base_method = base, how

    def _host_windows(self):
        return [(0, 0x100000000, self.base)]

    def _export_base(self):
        """Read g_debugGuestBase through the exe's PE export table inside the dump."""
        for name, mbase, _size in self.modules:
            if not name.lower().endswith(".exe"):
                continue
            hdr = self.read_host(mbase, 0x1000)
            if len(hdr) < 0x400 or hdr[:2] != b"MZ":
                continue
            lf = struct.unpack_from("<I", hdr, 0x3C)[0]
            if hdr[lf:lf + 4] != b"PE\0\0" or struct.unpack_from("<H", hdr, lf + 24)[0] != 0x20B:
                continue
            exp_rva, exp_size = struct.unpack_from("<II", hdr, lf + 24 + 112)
            if not exp_rva:
                continue
            ed = self.read_host(mbase + exp_rva, 40)
            if len(ed) < 40:
                continue
            nnames, afuncs, anames, aords = struct.unpack_from("<IIII", ed, 24)
            names = self.read_host(mbase + anames, 4 * nnames)
            ords = self.read_host(mbase + aords, 2 * nnames)
            for i in range(nnames):
                nrva = struct.unpack_from("<I", names, 4 * i)[0]
                nm = self.read_host(mbase + nrva, 32).split(b"\0")[0]
                if nm == b"g_debugGuestBase":
                    o = struct.unpack_from("<H", ords, 2 * i)[0]
                    frva = struct.unpack_from("<I", self.read_host(mbase + afuncs + 4 * o, 4), 0)[0]
                    ptr = self.read_host(mbase + frva, 8)
                    if len(ptr) == 8:
                        v = struct.unpack("<Q", ptr)[0]
                        return v or None
        return None

    def describe(self):
        return {"kind": self.kind, "path": os.path.abspath(self.path), "guest_base": "0x%X" % self.base,
                "base_method": self.base_method,
                "base_candidates": [[h, "0x%X" % b] for h, b in self.base_candidates]}


def open_image(spec, base=None):
    if spec.startswith("xenia:"):
        return XeniaImage(spec[6:])
    if spec.startswith("ours:"):
        return OursImage(spec[5:], base=base)
    if spec.startswith("snap:"):
        # the captured-pages image of tools/guest_snap.py (DEVTOOLS_DESIGN 6.2 step 5): a
        # SnapImage subclasses Image above; reads outside the captured pages come back short
        import guest_snap  # noqa: E402  (imports this module's Image; no cycle at import time)
        return guest_snap.SnapImage(spec[5:])
    raise ValueError("--image must be xenia:<name|path>, ours:<path> or snap:<file>")


# ================================================================= structure ====
def pe_sections(img):
    """Section table of the guest PE at 0x82000000 (.rdata holds every vtable)."""
    h = img.read(IMAGE_BASE_VA, 0x1000)
    if len(h) < 0x200 or h[:2] != b"MZ":
        return dict(FALLBACK_SECTIONS), False
    lf = struct.unpack_from("<I", h, 0x3C)[0]
    if lf + 24 > len(h) or h[lf:lf + 4] != b"PE\0\0":
        return dict(FALLBACK_SECTIONS), False
    nsec, = struct.unpack_from("<H", h, lf + 6)
    opt, = struct.unpack_from("<H", h, lf + 20)
    o = lf + 24 + opt
    out = {}
    for _ in range(nsec):
        if o + 40 > len(h):
            break
        nm = h[o:o + 8].rstrip(b"\0").decode("latin1")
        vs, va = struct.unpack_from("<II", h, o + 8)
        out.setdefault(nm, (IMAGE_BASE_VA + va, IMAGE_BASE_VA + va + vs))
        o += 40
    if ".rdata" not in out or ".data" not in out:
        return dict(FALLBACK_SECTIONS), False
    return out, True


def image_self_check(img):
    mz = img.read(IMAGE_BASE_VA, 2) == b"MZ"
    reg = img.cstr(img.u32(REGISTRY_MOPP_NAME_SLOT) or 0) == "hkMoppBvTreeShape"
    mgr = img.u32(G_PHYS_MGR)
    world = img.u32(mgr + 12) if mgr else None
    wvt = img.u32(world) if world else None
    return {"image_ok": bool(mz and reg), "mz": mz, "registry": reg,
            "phys_mgr": _h(mgr), "world": _h(world), "world_vt": _h(wvt), "world_ok": wvt == VT_HKWORLD}


def class_map(img, sections):
    """vtable -> class name from the Havok hkClass/vtable registry triples (name*, fn, vt) in .data."""
    lo, hi = sections[".data"]
    rlo, rhi = sections[".rdata"]
    runs = []                      # contiguous present runs of .data (a triple may cross a chunk edge)
    for va, data in img.iter_chunks(lo, hi):
        data = bytes(data)[max(0, lo - va): hi - va]
        va = max(va, lo)
        if runs and runs[-1][0] + len(runs[-1][1]) == va:
            runs[-1][1] += data
        else:
            runs.append([va, bytearray(data)])
    names = {}
    for va, buf in runs:
        for o in range((-va) & 3, len(buf) - 11, 4):
            nm, fn, vt = struct.unpack_from(">III", buf, o)
            if not (rlo <= vt < rhi and 0x82000000 <= nm < 0x83000000 and 0x82000000 <= fn < 0x83000000):
                continue
            s = img.cstr(nm, 64)
            if s and re.fullmatch(r"hk[A-Za-z0-9_]+", s):
                names[vt] = s
    out = dict(names)
    out.update(KNOWN_VT)
    return out


def _h(v):
    return None if v is None else "0x%08X" % v


def decode_filter_info(fi):
    """hkGroupFilter packing, as isCollisionEnabled sub_82578A68 reads it."""
    return {"layer": fi & 0x1F, "subsys": (fi >> 5) & 0x1F, "subsys_nocollide": (fi >> 10) & 0x1F,
            "group": fi >> 16}


def is_collision_enabled(a, b, table):
    """Port of sub_82578A68 (hkGroupFilter::isCollisionEnabled(u32,u32))."""
    if ((a ^ b) & 0xFFFF0000) != 0 or (a & 0xFFFF0000) == 0:
        return bool(table[a & 0x1F] & (1 << (b & 0x1F)))
    # same non-zero system group: subsystem ids (bits 5-9) vs don't-collide-with ids (bits 10-14)
    return ((((b >> 5) & 0xFFFF) ^ (a & 0xFFFF)) & 0x3E0) != 0 and \
        ((((a >> 5) & 0xFFFF) ^ (b & 0xFFFF)) & 0x3E0) != 0


def expected_game_filter_table():
    """The table the game filter ctor sub_822C49F8 builds (hkGroupFilter ctor sub_82578BA8 = all -1,
    disable sub_82578B70 / enable sub_82578B38 clear/set both (a,b) and (b,a))."""
    t = [0xFFFFFFFF] * 32

    def dis(a, b):
        t[a] &= ~(1 << b) & 0xFFFFFFFF
        t[b] &= ~(1 << a) & 0xFFFFFFFF

    def en(a, b):
        t[a] |= 1 << b
        t[b] |= 1 << a
    dis(17, 9); dis(17, 12); dis(6, 4); dis(7, 4); dis(6, 12)
    for i in range(1, 21):
        if i != 15:
            dis(15, i)
    en(15, 6); en(15, 7); en(15, 13)
    for i in range(1, 21):
        if i != 16:
            dis(16, i)
    for j in (2, 3, 6, 7, 13):
        en(16, j)
    for L in (1, 11, 5):
        for i in range(1, 21):
            if i != L:
                dis(L, i)
    for j in (9, 7, 6, 12):
        dis(9, j)
    for j in (6, 15, 16, 1, 9, 17, 10, 12):
        dis(18, j)
    for i in range(1, 21):
        if i != 12:
            dis(12, i)
    for j in (3, 4, 5, 2, 11, 13):
        en(12, j)
    for i in range(1, 21):
        if i != 19:
            dis(19, i)
    en(6, 19); en(3, 19); en(7, 19)
    for i in range(1, 21):
        if i != 8:
            dis(8, i)
    en(8, 15); en(8, 16)
    for i in range(1, 21):
        if i != 20:
            dis(20, i)
    en(6, 20); en(7, 20)
    return t


# ==================================================================== walker ====
class Walker:
    def __init__(self, img):
        self.img = img
        self.sections, self.pe_ok = pe_sections(img)
        self.rdata = self.sections[".rdata"]
        self.classes = class_map(img, self.sections)
        self.problems = []

    def cls(self, vt):
        if vt is None:
            return None
        return self.classes.get(vt, "unregistered_%08X" % vt)

    def vt_ok(self, vt):
        return vt is not None and self.rdata[0] <= vt < self.rdata[1]

    def arr(self, va, what):
        """hkArray {ptr, size, capacity|flags}."""
        p, n = self.img.u32(va), self.img.u32(va + 4)
        if p is None or n is None:
            self.problems.append("%s: hkArray at %s unreadable" % (what, _h(va)))
            return []
        if n > MAX_ARRAY:
            self.problems.append("%s: hkArray size %d implausible" % (what, n))
            return []
        if n and not p:
            self.problems.append("%s: hkArray size %d with null data" % (what, n))
            return []
        raw = self.img.read(p, 4 * n) if n else b""
        if len(raw) != 4 * n:
            self.problems.append("%s: hkArray data at %s short read" % (what, _h(p)))
            return []
        return list(struct.unpack(">%dI" % n, raw))

    # ---------------------------------------------------------------- pieces --
    def transform(self, t):
        u = {"transform": _h(t)}
        if not t:
            return u, ["no transform"]
        m = self.img.vec(t, 12)
        pos = self.img.vec(t + T_TRANSLATION, 3)
        bad = []
        if m is None or pos is None:
            return u, ["transform unreadable"]
        cols = [m[0:3], m[4:7], m[8:11]]
        for i in range(3):
            for j in range(3):
                d = sum(cols[i][k] * cols[j][k] for k in range(3))
                if not math.isfinite(d) or abs(d - (1.0 if i == j else 0.0)) > 2e-3:
                    bad.append("rotation not orthonormal")
                    break
            if bad:
                break
        if not all(math.isfinite(c) for c in pos):
            bad.append("position not finite")
        elif any(abs(c) > WORLD_LIMIT for c in pos):
            bad.append("position outside +-%g box" % WORLD_LIMIT)
        u["pos"] = [round(c, 4) for c in pos]
        return u, bad

    def shape(self, s):
        img = self.img
        if not s:
            return {"addr": None, "cls": None}, []
        vt = img.u32(s)
        d = {"addr": _h(s), "vt": _h(vt), "cls": self.cls(vt)}
        bad = [] if self.vt_ok(vt) else ["shape vtable %s outside .rdata" % _h(vt)]
        if vt == VT_MOPP:
            code, child = img.u32(s + MOPP_CODE), img.u32(s + MOPP_CHILD)
            cvt = img.u32(code) if code else None
            size, data = (img.u32(code + MC_SIZE), img.u32(code + MC_DATA)) if code else (None, None)
            d["mopp"] = {"code": _h(code), "code_vt_ok": cvt == VT_MOPP_CODE, "data": _h(data), "size": size,
                         "offset": img.vec(code + MC_OFFSET, 4) if code else None,
                         "child": _h(child), "child_cls": self.cls(img.u32(child)) if child else None}
            if cvt != VT_MOPP_CODE:
                bad.append("MOPP code vtable %s != hkMoppCode" % _h(cvt))
            elif size is None or size > (16 << 20) or not data:
                bad.append("MOPP code size/data implausible")
            else:
                raw = img.read(data, size)
                d["mopp"]["crc32"] = "%08X" % zlib.crc32(raw) if len(raw) == size else None
                if len(raw) != size:
                    bad.append("MOPP code bytes short read")
            if child and img.u32(child) == VT_LIST:
                d["mopp"]["child_count"] = img.u32(child + LIST_COUNT)
                d["mopp"]["child_filter"] = self.list_child_filter(child)
        elif vt == VT_LIST:
            n = img.u32(s + LIST_COUNT)
            d["list_count"] = n
            p = img.u32(s + LIST_CHILDREN)
            if n is not None and n <= 4096 and p:
                kinds = Counter(self.cls(img.u32(img.u32(p + 8 * i) or 0)) for i in range(n))
                d["list_children"] = dict(kinds)
                d["list_child_filter"] = self.list_child_filter(s)
            else:
                bad.append("list shape child array implausible")
        return d, bad

    def list_child_filter(self, s):
        """Multiset of the per-child collision filterInfo words (hkListShape ChildInfo {shape, filterInfo},
        stride 8 at +0x10). The narrowphase filters EVERY child key with it: the shape-key filter
        hkGroupFilter_isCollisionEnabled_shapeKey_sub_82578CB0 compares the other body's filterInfo with
        collection->getCollisionFilterInfo(key) (vt+44), not with the body's own filterInfo. A changed
        child word can therefore disable collision with a building whose body filterInfo and MOPP CRC
        both match (iter-463 collision map). Xenia goldens: all layer-7 child words are 0."""
        img = self.img
        n, p = img.u32(s + LIST_COUNT), img.u32(s + LIST_CHILDREN)
        if n is None or n > 4096 or not p:
            return None
        return dict(Counter("%08X" % (img.u32(p + 8 * i + 4) or 0) for i in range(n)))

    def bp_aabb(self, bp, bid, handle_va):
        """AABB from the hk3AxisSweep node, with endpoint->node backrefs checked."""
        img = self.img
        if not bp or bid is None:
            return None, "no broadphase"
        nodes = img.u32(bp + BP_NODES)
        nn = img.u32(bp + BP_NODES + 4)
        if nodes is None or nn is None or bid >= nn:
            return None, "bp id %s outside node array (%s)" % (bid, nn)
        raw = img.read(nodes + 16 * bid, 16)
        if len(raw) != 16:
            return None, "bp node unreadable"
        miny, minz, maxy, maxz, minx, maxx, h = struct.unpack(">6HI", raw)
        if h != handle_va:
            return None, "bp node %d handle %s != collidable handle %s" % (bid, _h(h), _h(handle_va))
        axes = [img.u32(bp + a) for a in BP_AXES]
        off = img.vec(bp + BP_OFFSET_LOW, 3)
        sc = img.vec(bp + BP_SCALE, 3)
        if None in axes or off is None or sc is None or 0.0 in sc:
            return None, "broadphase header unreadable"
        vals = []
        for ax, idx in ((0, minx), (1, miny), (2, minz), (0, maxx), (1, maxy), (2, maxz)):
            e = img.read(axes[ax] + 4 * idx, 4)
            if len(e) != 4:
                return None, "bp endpoint unreadable"
            v, node = struct.unpack(">HH", e)
            if node != bid:
                return None, "bp endpoint backref %d != node %d" % (node, bid)
            vals.append(v / sc[ax] - off[ax])
        return [[round(x, 2) for x in vals[:3]], [round(x, 2) for x in vals[3:]]], None

    def body(self, e, world, island, kind, bp):
        img = self.img
        vt = img.u32(e)
        fi = img.u32(e + E_FILTER_INFO)
        motion = img.u32(e + E_MOTION)
        b = {"addr": _h(e), "vt": _h(vt), "cls": self.cls(vt), "island": kind, "island_addr": _h(island),
             "name": img.cstr(img.u32(e + E_NAME) or 0), "user_data": _h(img.u32(e + E_USERDATA)),
             "filter_info": _h(fi), "fixed_flag": img.u8(e + E_FIXED),
             "motion": _h(motion), "motion_cls": self.cls(img.u32(motion)) if motion else None,
             "bp_id": img.u32(e + E_BP_ID), "bp_type": img.u8(e + E_BP_TYPE)}
        bad = []
        if not self.vt_ok(vt):
            bad.append("entity vtable %s outside .rdata" % _h(vt))
        if fi is not None:
            b.update(decode_filter_info(fi))
        if img.u32(e + E_WORLD) != world:
            bad.append("entity.world %s != world" % _h(img.u32(e + E_WORLD)))
        if img.u32(e + E_ISLAND) != island:
            bad.append("entity.island %s != walked island" % _h(img.u32(e + E_ISLAND)))
        if motion and not self.vt_ok(img.u32(motion)):
            bad.append("motion vtable outside .rdata")
        t, tb = self.transform(img.u32(e + E_TRANSFORM))
        b.update(t)
        bad += tb
        sd, sb = self.shape(img.u32(e + E_SHAPE))
        b["shape"] = sd
        bad += sb
        if sd.get("addr"):
            aabb, why = self.bp_aabb(bp, b["bp_id"], e + E_BP_HANDLE)
            b["aabb"] = aabb
            if why:
                bad.append("broadphase: " + why)
        b["static"] = kind == "fixed" or b["motion_cls"] == "hkFixedRigidMotion"
        b["problems"] = bad
        return b

    # ------------------------------------------------------------------ world --
    def run(self):
        img = self.img
        snap = {"schema": SCHEMA, "image": img.describe(), "self_check": image_self_check(img),
                "pe_sections_from_image": self.pe_ok}
        mgr = img.u32(G_PHYS_MGR)
        world = img.u32(mgr + 12) if mgr else None
        snap["phys_mgr"], snap["world_addr"] = _h(mgr), _h(world)
        if not world or img.u32(world) != VT_HKWORLD:
            snap["error"] = "no hkWorld at *(*(0x8296F244)+12) (vt %s)" % _h(img.u32(world) if world else None)
            snap["globals"] = self.globals(None)
            snap["problems"] = self.problems
            return snap
        sim = img.u32(world + W_SIMULATION)
        bp = img.u32(world + W_BROADPHASE)
        bp_ok = img.u32(bp) == VT_3AXIS if bp else False
        if not bp_ok:
            self.problems.append("broadphase %s vt %s is not hk3AxisSweep" % (_h(bp), _h(img.u32(bp) if bp else None)))
        snap["world"] = {
            "vt": _h(img.u32(world)), "gravity": img.vec(world + W_GRAVITY, 3),
            "simulation": _h(sim), "fixed_body": _h(img.u32(world + W_FIXED_BODY)),
            "broadphase": _h(bp), "broadphase_ok": bp_ok,
            "broadphase_offset_low": img.vec(bp + BP_OFFSET_LOW, 3) if bp_ok else None,
            "broadphase_scale": img.vec(bp + BP_SCALE, 3) if bp_ok else None,
            "memory_watchdog": _h(img.u32(world + W_WATCHDOG)),
            "critical_lock_count": img.u32(world + W_CRIT_LOCK),
            "critical_lock_count_phantom": img.u32(world + W_CRIT_LOCK_PHANTOM),
            "block_pending_ops": img.u8(world + W_BLOCK_PENDING),
            "op_queue": _h(img.u32(world + W_OP_QUEUE)),
            "pending_ops": img.u32(world + W_PENDING_OPS),
        }
        snap["globals"] = self.globals(sim)
        snap["filter"] = self.filter(img.u32(world + W_FILTER))
        # islands
        islands = [("fixed", 0, img.u32(world + W_FIXED_ISLAND))]
        islands += [("active", i, a) for i, a in enumerate(self.arr(world + W_ACTIVE_ISLANDS, "active islands"))]
        islands += [("inactive", i, a) for i, a in enumerate(self.arr(world + W_INACTIVE_ISLANDS, "inactive islands"))]
        bodies, isl_out = [], []
        for kind, idx, isl in islands:
            ivt = img.u32(isl) if isl else None
            rec = {"kind": kind, "index": idx, "addr": _h(isl), "vt_ok": ivt == VT_ISLAND,
                   "world_ok": img.u32(isl + I_WORLD) == world if isl else False,
                   "active_flag": img.u8(isl + I_ACTIVE) if isl else None}
            if not rec["vt_ok"] or not rec["world_ok"]:
                self.problems.append("island %s[%d] %s: vt %s world-backref %s" % (kind, idx, _h(isl), _h(ivt),
                                                                                  rec["world_ok"]))
            ents = self.arr(isl + I_ENTITIES, "island %s[%d] entities" % (kind, idx)) if isl else []
            rec["entities"] = len(ents)
            isl_out.append(rec)
            for e in ents:
                bodies.append(self.body(e, world, isl, kind, bp if bp_ok else None))
        snap["islands"] = isl_out
        snap["bodies"] = bodies
        snap["phantoms"] = self.phantoms(world)
        snap["character_proxies"], snap["superman"] = self.proxies(world, snap["phantoms"], snap["filter"])
        snap["problems"] = self.problems
        return snap

    def globals(self, sim):
        img = self.img
        hm = img.u32(G_HK_MEMORY)
        lp = img.u32(G_LOOP_MGR)
        g = {"hk_memory": _h(hm),
             "hk_memory_failure_flag": img.u32(hm + 4) if hm else None,
             "hk_memory_soft_limit": _h(img.u32(hm + 8)) if hm else None,
             "loop_mgr": _h(lp)}
        if lp:
            hi, lo = img.u32(lp + 0x48), img.u32(lp + 0x4C)
            g.update({"loop_step_mode": img.u32(lp + 0x34),
                      "loop_period_ns": (hi << 32 | lo) if hi is not None and lo is not None else None,
                      "loop_dt84": img.f32(lp + 0x84)})
        if sim:
            g.update({"simulation_vt": _h(img.u32(sim)), "simulation_last_dt": img.f32(sim + SIM_LAST_DT),
                      "simulation_prev_result": img.u32(sim + SIM_PREV_RESULT),
                      "simulation_status": img.u32(sim + SIM_STATUS)})
        return g

    def filter(self, f):
        img = self.img
        if not f:
            self.problems.append("world has no collision filter")
            return {"addr": None}
        vt = img.u32(f)
        table = [img.u32(f + F_TABLE + 4 * i) for i in range(32)]
        d = {"addr": _h(f), "vt": _h(vt), "cls": self.cls(vt), "size_ref": _h(img.u32(f + 4)),
             "next_free_group": img.u32(f + F_NEXT_FREE_GROUP), "table": [_h(w) for w in table]}
        if None in table:
            self.problems.append("filter table unreadable")
            return d
        d["symmetric"] = all(((table[a] >> b) & 1) == ((table[b] >> a) & 1) for a in range(32) for b in range(32))
        if vt == VT_GAME_FILTER:
            exp = expected_game_filter_table()
            d["ctor_table_diff"] = [[i, _h(exp[i]), _h(table[i])] for i in range(32) if exp[i] != table[i]]
        d["disabled_pairs_by_layer"] = {str(L): [j for j in range(21) if not (table[L] >> j) & 1] for L in range(21)}
        return d

    def phantoms(self, world):
        img = self.img
        out = []
        for p in self.arr(world + W_PHANTOMS, "world phantoms"):
            vt = img.u32(p)
            fi = img.u32(p + P_FILTER_INFO)
            s = img.u32(p + P_SHAPE)
            t = img.u32(p + P_TRANSFORM)
            d = {"addr": _h(p), "vt": _h(vt), "cls": self.cls(vt), "filter_info": _h(fi),
                 "shape": _h(s), "shape_cls": self.cls(img.u32(s)) if s else None,
                 "pos": [round(c, 4) for c in img.vec(t + T_TRANSLATION, 3)] if t else None}
            if fi is not None:
                d.update(decode_filter_info(fi))
            if not self.vt_ok(vt):
                self.problems.append("phantom %s vtable outside .rdata" % _h(p))
            out.append(d)
        return out

    def proxies(self, world, phantoms, filt):
        """Every hkCharacterProxy (vtable-triple scan) and the one owned by Superman's actor."""
        img = self.img
        rlo, rhi = self.rdata
        in_world = {ph["addr"] for ph in phantoms}
        hits = img.scan_u32([VT_CHAR_PROXY, VT_PRXY_OBJ, VT_ACTOR], 0, 0x100000000)
        scan = lambda vt: [a for a in hits[vt] if not (rlo <= a < rhi)]
        prox = [a for a in scan(VT_CHAR_PROXY)
                if img.u32(a + 8) == VT_CHAR_PROXY_2 and img.u32(a + 12) == VT_CHAR_PROXY_3]
        prxy = {img.u32(a + PRXY_PROXY): a for a in scan(VT_PRXY_OBJ)}
        actors = {img.u32(a + ACTOR_PHYS_CHILD): a for a in scan(VT_ACTOR)}
        table = None
        if filt.get("table") and None not in filt["table"]:
            table = [int(x, 16) for x in filt["table"]]
        out, superman = [], None
        for a in prox:
            ph = img.u32(a + CP_PHANTOM)
            fi = img.u32(ph + P_FILTER_INFO) if ph else None
            t = img.u32(ph + P_TRANSFORM) if ph else None
            owner = prxy.get(a)
            actor = actors.get(owner) if owner else None
            handler = img.u32(actor + ACTOR_HANDLER) if actor else None
            fsm = img.cstr(img.u32(handler + HANDLER_NAME) or 0) if handler else None
            mptr, mn = img.u32(a + CP_MANIFOLD), img.u32(a + CP_MANIFOLD + 4)
            manifold = []
            if mptr and mn is not None and mn <= 256:
                for i in range(mn):
                    ca, cb = img.u32(mptr + 48 * i + RCP_COLL_A), img.u32(mptr + 48 * i + RCP_COLL_B)
                    manifold.append({"pos": img.vec(mptr + 48 * i, 3), "normal_dist": img.vec(mptr + 48 * i + 16, 4),
                                     "collidable_a": _h(ca), "collidable_b": _h(cb),
                                     "body_b": _h(cb - E_COLLIDABLE) if cb else None,
                                     "body_b_name": img.cstr(img.u32(cb - E_COLLIDABLE + E_NAME) or 0) if cb else None,
                                     "body_b_filter_info": _h(img.u32(cb + 0x1C)) if cb else None})
            d = {"addr": _h(a), "phantom": _h(ph), "phantom_in_world": _h(ph) in in_world,
                 "phantom_cls": self.cls(img.u32(ph)) if ph else None, "filter_info": _h(fi),
                 "shape_cls": self.cls(img.u32(img.u32(ph + P_SHAPE) or 0)) if ph else None,
                 "pos": [round(c, 4) for c in img.vec(t + T_TRANSLATION, 3)] if t else None,
                 "prxy_obj": _h(owner), "actor": _h(actor), "fsm": fsm, "manifold_n": mn,
                 "manifold": manifold,
                 "bodies": [_h(x) for x in self.arr(a + CP_BODIES, "proxy bodies")],
                 "phantoms": [_h(x) for x in self.arr(a + CP_PHANTOMS, "proxy phantoms")]}
            if fi is not None:
                d.update(decode_filter_info(fi))
            out.append(d)
            if fsm and fsm.upper().startswith("SUPERMAN"):
                if superman is not None:
                    self.problems.append("more than one SUPERMAN* proxy")
                superman = d
        if superman is None:
            self.problems.append("Superman's character proxy not identified (actor FSM name SUPERMAN*)")
        else:
            superman["method"] = "hkCharacterProxy -> PhysCharProxyObj(+4) -> actor(+0x70) -> FSM name SUPERMAN*"
            if table is not None and superman.get("filter_info"):
                sfi = int(superman["filter_info"], 16)
                superman["filter_blocks_layers"] = [L for L in range(32) if not table[sfi & 0x1F] >> L & 1]
        return out, superman


# =================================================================== reports ====
def point_aabb_dist(p, b):
    a = b.get("aabb")
    if a:
        lo, hi = a
        return math.sqrt(sum(max(lo[k] - p[k], 0.0, p[k] - hi[k]) ** 2 for k in range(3)))
    if b.get("pos"):
        return math.dist(p, b["pos"])
    return float("inf")


def near_bodies(snap, p, r):
    table = snap.get("filter", {}).get("table")
    tbl = [int(x, 16) for x in table] if table and None not in table else None
    sm = snap.get("superman") or {}
    sfi = int(sm["filter_info"], 16) if sm.get("filter_info") else None
    out = []
    for b in snap.get("bodies", []):
        d = point_aabb_dist(p, b)
        if d <= r:
            row = dict(b)
            row["dist"] = round(d, 2)
            if tbl is not None and sfi is not None and b.get("filter_info"):
                row["collides_with_superman"] = is_collision_enabled(sfi, int(b["filter_info"], 16), tbl)
            out.append(row)
    out.sort(key=lambda r_: r_["dist"])
    return out


def shape_kind(b):
    return (b.get("shape") or {}).get("cls") or "none"


def summary_text(snap, p, r):
    L = []
    w = L.append
    im = snap["image"]
    w("havok_snapshot %s  image=%s" % (snap["schema"], json.dumps(im)))
    w("self_check: %s" % json.dumps(snap["self_check"]))
    if snap.get("error"):
        w("ERROR: " + snap["error"])
        w("globals: " + json.dumps(snap.get("globals")))
        return "\n".join(L)
    wd = snap["world"]
    w("hkWorld %s gravity %s broadphase %s (ok %s, offLow %s scale %s)" % (
        snap["world_addr"], wd["gravity"], wd["broadphase"], wd["broadphase_ok"], wd["broadphase_offset_low"],
        wd["broadphase_scale"]))
    w("world: watchdog %s  critical_lock_count %s (phantom %s)  block_pending_ops %s  pending_ops %s" % (
        wd["memory_watchdog"], wd["critical_lock_count"], wd.get("critical_lock_count_phantom"),
        wd.get("block_pending_ops"), wd["pending_ops"]))
    w("globals: " + json.dumps(snap["globals"]))
    kinds = Counter(i["kind"] for i in snap["islands"])
    ents = defaultdict(int)
    for i in snap["islands"]:
        ents[i["kind"]] += i["entities"]
    w("islands: %s  entities: %s" % (dict(kinds), dict(ents)))
    bodies = snap["bodies"]
    w("bodies: %d (static %d, moving %d)" % (len(bodies), sum(b["static"] for b in bodies),
                                            sum(not b["static"] for b in bodies)))
    bl = Counter(b.get("layer") for b in bodies)
    w("by layer: " + ", ".join("L%s=%d" % (k, v) for k, v in sorted(bl.items(), key=lambda x: (x[0] is None, x[0]))))
    w("by shape: " + ", ".join("%s=%d" % kv for kv in Counter(shape_kind(b) for b in bodies).most_common()))
    w("by motion: " + ", ".join("%s=%d" % kv for kv in Counter(b.get("motion_cls") for b in bodies).most_common()))
    for layer in (7, 3, 13):
        c = Counter((b["island"], shape_kind(b)) for b in bodies if b.get("layer") == layer)
        w("layer %d: %d  %s" % (layer, sum(c.values()), ", ".join("%s/%s=%d" % (k[0], k[1], v)
                                                              for k, v in c.most_common())))
    mopp = [b for b in bodies if (b.get("shape") or {}).get("mopp")]
    w("MOPP bodies %d, code_vt_ok %d, crc'd %d" % (len(mopp), sum(b["shape"]["mopp"]["code_vt_ok"] for b in mopp),
                                                   sum(bool(b["shape"]["mopp"].get("crc32")) for b in mopp)))
    bad = [b for b in bodies if b["problems"]]
    w("bodies failing structural checks: %d" % len(bad))
    for b in bad[:25]:
        w("   %s %-22s L%s %-24s %s" % (b["addr"], b.get("name"), b.get("layer"), shape_kind(b), "; ".join(b["problems"])))
    f = snap["filter"]
    w("filter %s vt %s (%s) next_free_group %s symmetric %s" % (f.get("addr"), f.get("vt"), f.get("cls"),
                                                               f.get("next_free_group"), f.get("symmetric")))
    w("  table: " + " ".join((x or "????????")[2:] for x in f.get("table", [])))
    if "ctor_table_diff" in f:
        w("  words differing from the ctor sub_822C49F8 table: %s" % (f["ctor_table_diff"] or "none"))
    for Lr in ("2", "3", "7", "13"):
        if f.get("disabled_pairs_by_layer"):
            w("  layer %s does NOT collide with %s" % (Lr, f["disabled_pairs_by_layer"][Lr]))
    ph = snap["phantoms"]
    w("phantoms %d: %s" % (len(ph), ", ".join("%s/L%s=%d" % (k[0], k[1], v) for k, v in
                                               Counter((x["cls"], x.get("layer")) for x in ph).most_common())))
    cps = snap["character_proxies"]
    w("character proxies: %d (in world: %d)" % (len(cps), sum(c["phantom_in_world"] for c in cps)))
    sm = snap.get("superman")
    if sm:
        w("SUPERMAN proxy %s phantom %s (in world %s, %s) filterInfo %s layer %s group %s subsys %s/%s" % (
            sm["addr"], sm["phantom"], sm["phantom_in_world"], sm["shape_cls"], sm["filter_info"], sm.get("layer"),
            sm.get("group"), sm.get("subsys"), sm.get("subsys_nocollide")))
        w("  pos %s  actor %s  fsm %s" % (sm["pos"], sm["actor"], sm["fsm"]))
        w("  manifold %s: %s" % (sm["manifold_n"], "; ".join("%s %s fi %s n/d %s" % (
            m["body_b"], m["body_b_name"], m["body_b_filter_info"],
            [round(x, 3) for x in m["normal_dist"]] if m["normal_dist"] else None) for m in sm["manifold"])))
        w("  listened bodies %s  phantoms %s" % (sm["bodies"], sm["phantoms"]))
    else:
        w("SUPERMAN proxy: NOT FOUND")
    if p is not None:
        nb = near_bodies(snap, p, r)
        w("bodies within %.1f of %s (AABB distance): %d" % (r, [round(x, 2) for x in p], len(nb)))
        for b in nb[:80]:
            w("   d=%7.2f %s %-8s L%-2s %-22s %-24s pos %s aabb %s collides_with_superman=%s%s" % (
                b["dist"], b["addr"], b["island"], b.get("layer"), (b.get("name") or "")[:22], shape_kind(b),
                b.get("pos"), b.get("aabb"), b.get("collides_with_superman"),
                "  PROBLEMS: " + "; ".join(b["problems"]) if b["problems"] else ""))
    if snap["problems"]:
        w("walk problems (%d): %s" % (len(snap["problems"]), snap["problems"][:20]))
    return "\n".join(L)


# ====================================================================== diff ====
def _finite_pos(b):
    p = b.get("pos")
    if not p or len(p) != 3 or not all(isinstance(c, (int, float)) and math.isfinite(c) for c in p):
        return None
    return p


def body_key(b, q=0.1):
    """(layer, shape class, position bucket). A body without a finite position (no transform,
    NaN, inf) cannot be placed: it gets its own ('nopos', addr) bucket so it is reported, never
    crashes the diff and never pairs by accident (verifier finding, iter-463)."""
    p = _finite_pos(b)
    if p is None:
        return (b.get("layer"), shape_kind(b), ("nopos", b.get("addr")))
    return (b.get("layer"), shape_kind(b), tuple(round(c / q) for c in p))


def body_sig(b):
    """Identity fields compared on every matched pair (beyond the key's layer/shape/position).
    Positions are world-space (collidable.m_motion -> the motion's world hkTransform), so no
    parent-relative composition is needed; hkTransformShape / hkConvexTranslateShape children
    carry their local offsets INSIDE the shape and are covered by the shape fields below."""
    sh = b.get("shape") or {}
    m = sh.get("mopp") or {}
    return {"filter_info": b.get("filter_info"), "name": b.get("name"), "cls": b.get("cls"),
            "motion_cls": b.get("motion_cls"), "fixed_flag": b.get("fixed_flag"), "bp_type": b.get("bp_type"),
            "mopp_crc32": m.get("crc32"), "mopp_size": m.get("size"), "mopp_child_cls": m.get("child_cls"),
            "mopp_child_count": m.get("child_count"), "list_count": sh.get("list_count"),
            "list_children": json.dumps(sh.get("list_children"), sort_keys=True),
            "child_filter": json.dumps(sh.get("list_child_filter") or m.get("child_filter"), sort_keys=True),
            "problems": len(b.get("problems") or [])}


def sig_diff(x, y):
    sx, sy = body_sig(x), body_sig(y)
    # child_filter was added after the first snapshots were written: a side without it ("null")
    # is unknown, not different.
    return [k for k in sx if sx[k] != sy[k] and not (k == "child_filter" and "null" in (sx[k], sy[k]))]


def _pair_group(la, lb):
    """Pair two same-key lists: identical signatures first, then the rest in order (reported as mismatches)."""
    lb = list(lb)
    pairs, rest_a = [], []
    for x in la:
        sx = body_sig(x)
        j = next((i for i, y in enumerate(lb) if body_sig(y) == sx), None)
        if j is None:
            rest_a.append(x)
        else:
            pairs.append((x, lb.pop(j)))
    n = min(len(rest_a), len(lb))
    pairs += list(zip(rest_a[:n], lb[:n]))
    return pairs, rest_a[n:], lb[n:]


def _nearest(x, cands, tol, same_kind=True):
    px = _finite_pos(x)
    if px is None:
        return None
    best, bd = None, None
    for y in cands:
        py = _finite_pos(y)
        if py is None or (same_kind and (x.get("layer") != y.get("layer") or shape_kind(x) != shape_kind(y))):
            continue
        d = max(abs(px[i] - py[i]) for i in range(3))
        if d <= tol and (bd is None or d < bd or (d == bd and body_sig(y) == body_sig(x))):
            best, bd = y, d
    return best


def diff_snapshots(a, b, p=None, r=None, tol=0.25):
    """Static bodies as multisets keyed by (layer, shape, pos rounded 0.1). Same-key groups pair
    identical signatures first; leftovers re-pair with the NEAREST same layer+shape body within
    `tol` per axis (bucket-edge / ulp drift). Leftover statics are then tried against the other
    side's MOVING bodies (a building that became dynamic = status_change, not "missing").
    Matching runs on the FULL sets and the radius scope is applied afterwards (a pair is kept
    if either side is within r), so AABB quantization at the radius edge cannot fake a miss.
    Every matched pair compares body_sig; bodies with structural problems are listed per side."""
    inside = (lambda x: True) if p is None else (lambda x: point_aabb_dist(p, x) <= r)
    A, B = a.get("bodies", []), b.get("bodies", [])
    As, Bs = [x for x in A if x["static"]], [x for x in B if x["static"]]
    ka, kb = defaultdict(list), defaultdict(list)
    for x in As:
        ka[body_key(x)].append(x)
    for x in Bs:
        kb[body_key(x)].append(x)
    pairs, onlyA, onlyB = [], [], []
    for k in sorted(set(ka) | set(kb), key=repr):
        pp, ra, rb = _pair_group(ka.get(k, []), kb.get(k, []))
        pairs += pp
        onlyA += ra
        onlyB += rb
    rest_b = list(onlyB)
    still_a = []
    for x in onlyA:
        hit = _nearest(x, rest_b, tol)
        if hit is not None:
            rest_b.remove(hit)
            pairs.append((x, hit))
        else:
            still_a.append(x)
    status = []
    mov_a, mov_b = [x for x in A if not x["static"]], [x for x in B if not x["static"]]
    for lst, other, side in ((still_a, mov_b, "a"), (rest_b, mov_a, "b")):
        for x in list(lst):
            hit = _nearest(x, other, tol)
            if hit is not None:
                lst.remove(x)
                other.remove(hit)
                status.append((x, hit) if side == "a" else (hit, x))
    pairs = [(x, y) for x, y in pairs if inside(x) or inside(y)]
    mismatch = [(x, y, sig_diff(x, y)) for x, y in pairs if sig_diff(x, y)]
    moving = {"a": Counter((x.get("layer"), shape_kind(x)) for x in A if not x["static"] and inside(x)),
              "b": Counter((x.get("layer"), shape_kind(x)) for x in B if not x["static"] and inside(x))}
    return {"matched": len(pairs), "only_a": [x for x in still_a if inside(x)],
            "only_b": [x for x in rest_b if inside(x)],
            "field_mismatch": [(x, y) for x, y, _ in mismatch], "field_mismatch_fields": [f for _, _, f in mismatch],
            "status_change": [(x, y) for x, y in status if inside(x) or inside(y)],
            "problems_a": [x for x in A if x.get("problems") and inside(x)],
            "problems_b": [x for x in B if x.get("problems") and inside(x)],
            "moving": moving}


def diff_text(a, b, names, p, r):
    L = []
    w = L.append
    w("havok_snapshot diff  A=%s  B=%s" % names)
    w("scope: %s" % ("all bodies" if p is None else "AABB within %.1f of %s" % (r, p)))
    d = diff_snapshots(a, b, p, r)
    w("static bodies matched %d; only in A %d; only in B %d; matched-but-different %d; static<->moving %d; "
      "with structural problems A %d B %d" % (
          d["matched"], len(d["only_a"]), len(d["only_b"]), len(d["field_mismatch"]), len(d["status_change"]),
          len(d["problems_a"]), len(d["problems_b"])))
    for tag, lst in (("ONLY IN A", d["only_a"]), ("ONLY IN B", d["only_b"])):
        by = Counter((x.get("layer"), shape_kind(x)) for x in lst)
        w("%s by layer/shape: %s" % (tag, dict(by)))
        for x in sorted(lst, key=lambda x_: (x_.get("layer") or 0, _finite_pos(x_) or []))[:60]:
            w("   L%-2s %-24s %-22s pos %s aabb %s" % (x.get("layer"), shape_kind(x), (x.get("name") or "")[:22],
                                                    x.get("pos"), x.get("aabb")))
    by_field = Counter(f for fs in d["field_mismatch_fields"] for f in fs)
    w("matched-but-different by field: %s" % dict(by_field))
    for (x, y), fs in list(zip(d["field_mismatch"], d["field_mismatch_fields"]))[:30]:
        sx, sy = body_sig(x), body_sig(y)
        w("   DIFF L%s %s pos %s: %s" % (x.get("layer"), x.get("name"), x.get("pos"),
                                         "; ".join("%s A=%s B=%s" % (f, sx[f], sy[f]) for f in fs)))
    for x, y in d["status_change"][:30]:
        w("   STATUS L%s %s pos %s: A %s/%s | B %s/%s" % (x.get("layer"), x.get("name"), x.get("pos"),
                                                         x.get("island"), x.get("motion_cls"), y.get("island"),
                                                         y.get("motion_cls")))
    for tag, lst in (("A", d["problems_a"]), ("B", d["problems_b"])):
        for x in lst[:30]:
            w("   PROBLEMS %s %s L%s %s: %s" % (tag, x.get("addr"), x.get("layer"), x.get("name"),
                                              "; ".join(x["problems"])))
    w("moving bodies (expected to differ): A %s | B %s" % (dict(d["moving"]["a"]), dict(d["moving"]["b"])))
    fa, fb = a.get("filter", {}), b.get("filter", {})
    ta, tb = fa.get("table") or [], fb.get("table") or []
    dt = [i for i in range(min(len(ta), len(tb))) if ta[i] != tb[i]]
    w("filter: vt A %s B %s; table words differing: %s" % (fa.get("vt"), fb.get("vt"),
                                                           [(i, ta[i], tb[i]) for i in dt] or "none"))
    ga, gb = a.get("globals", {}), b.get("globals", {})
    for k in sorted(set(ga) | set(gb)):
        if k in ("hk_memory", "loop_mgr", "simulation_vt"):
            continue
        va, vb = ga.get(k), gb.get(k)
        w("  %-26s A=%-14s B=%-14s %s" % (k, va, vb, "" if va == vb else "<-- DIFFERS"))
    for k in ("critical_lock_count", "critical_lock_count_phantom", "block_pending_ops", "pending_ops",
              "memory_watchdog", "gravity"):
        va, vb = a.get("world", {}).get(k), b.get("world", {}).get(k)
        w("  world.%-20s A=%-14s B=%-14s %s" % (k, va, vb, "" if va == vb else "<-- DIFFERS"))
    sa, sb = a.get("superman") or {}, b.get("superman") or {}
    for k in ("filter_info", "layer", "group", "subsys", "subsys_nocollide", "shape_cls", "phantom_in_world",
              "manifold_n"):
        va, vb = sa.get(k), sb.get(k)
        w("  superman.%-17s A=%-14s B=%-14s %s" % (k, va, vb, "" if va == vb else "<-- DIFFERS"))
    w("  superman.pos A=%s B=%s" % (sa.get("pos"), sb.get("pos")))
    pa = Counter((x["cls"], x.get("layer")) for x in a.get("phantoms", []))
    pb = Counter((x["cls"], x.get("layer")) for x in b.get("phantoms", []))
    w("phantoms A %s | B %s" % (dict(pa), dict(pb)))
    return "\n".join(L)


# ======================================================================= CLI ====
def parse_point(s):
    if s is None or s in ("a", "b"):
        return s   # diff: "a"/"b" = that snapshot's Superman position
    v = [float(x) for x in s.split(",")]
    if len(v) != 3:
        raise argparse.ArgumentTypeError("--near wants x,y,z")
    return v


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("snap")
    s.add_argument("--image", required=True, help="xenia:<golden name|path> | ours:<minidump path>")
    s.add_argument("--base", type=lambda x: int(x, 0), help="ours: guest base (checked, not trusted)")
    s.add_argument("--label")
    s.add_argument("--out-dir", default=DEFAULT_OUT)
    s.add_argument("--near", type=parse_point)
    s.add_argument("--radius", type=float, default=150.0)
    d = sub.add_parser("diff")
    d.add_argument("a")
    d.add_argument("b")
    d.add_argument("--near", type=parse_point)
    d.add_argument("--radius", type=float, default=150.0)
    d.add_argument("--out")
    args = ap.parse_args(argv)

    if args.cmd == "snap":
        img = open_image(args.image, args.base)
        snap = Walker(img).run()
        p = args.near if isinstance(args.near, list) else ((snap.get("superman") or {}).get("pos"))
        snap["query"] = {"near": p, "radius": args.radius}
        label = args.label or re.sub(r"[^A-Za-z0-9_.-]+", "_", args.image.split(":", 1)[1].split("/")[-1].split("\\")[-1])
        os.makedirs(args.out_dir, exist_ok=True)
        jp = os.path.join(args.out_dir, label + ".json")
        tp = os.path.join(args.out_dir, label + ".txt")
        with open(jp, "w") as fh:
            json.dump(snap, fh, indent=1)
        txt = summary_text(snap, p, args.radius)
        with open(tp, "w") as fh:
            fh.write(txt + "\n")
        print(txt)
        print("\nwrote %s\n      %s" % (jp, tp))
        return 0 if not snap.get("error") else 2
    a, b = (json.load(open(x)) for x in (args.a, args.b))
    p = args.near
    if p in ("a", "b"):
        p = ((a if p == "a" else b).get("superman") or {}).get("pos")
        if p is None:
            print("snapshot %s has no Superman position; use --near=x,y,z" % args.near)
            return 2
    txt = diff_text(a, b, (args.a, args.b), p, args.radius)
    out = args.out or os.path.join(os.path.dirname(os.path.abspath(args.a)), "%s_vs_%s.txt" % (
        os.path.splitext(os.path.basename(args.a))[0], os.path.splitext(os.path.basename(args.b))[0]))
    with open(out, "w") as fh:
        fh.write(txt + "\n")
    print(txt)
    print("\nwrote " + out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
