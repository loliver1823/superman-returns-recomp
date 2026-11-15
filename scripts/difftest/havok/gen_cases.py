#!/usr/bin/env python3
"""gen_cases.py - generate golden_call scripts for the Havok collision differential test
(scripts/difftest/golden_call: recomp harness vs clean-room interpreter oracle, same golden memory).

Order: `types` first (its recomp output feeds every other set's dispatch), `aabb` before `ray`.
Sets (all on the sound_gameplay golden memory, layer-7 BUILDING bodies from the havok_snapshot):
  types: getType (vt+8) of every building / MOPP child / terrain shape + Superman's capsule
  aabb: getAabb (vt+12; FP arg in f1 so the out pointer is r6) under identity, tol 0 / 0.1
  ray : castRay (vt+20) in shape space through / near / grazing every unique building shape
  ct  : hkCapsuleTriangleClosestPoints 0x825C3C08 directly on synthetic capsule/triangle pairs
  ig  : the full hkCharacterProxy::integrate 0x825EC5A8 of Superman's golden proxy after teleporting
        it next to a building with a velocity into it (broadphase, narrowphase, simplex solver,
        manifold, the game's own contact listeners)
  mq  : hkMoppBvTreeShape::queryAabb (vt slot 9, 0x82577A58 -> MOPP OBB VM) on every unique MOPP
        building shape: AABBs inside / around / grazing / degenerate / quantization-boundary.
  lc  : Superman's capsule collidable (phantom 0x022E1A40, collidable +0x14) linear-cast against
        building bodies through the golden collision dispatcher (capsule row, linearCast slot):
        MOPP -> 0x8258B920, list/mesh -> 0x82585E48, box/convex -> 0x82593570.
  cp  : the same pairs through getClosestPoints (slot +8): capsule placed in / near the walls.
  gold: Superman's EXACT golden state vs the terrain body 0x022ADF40 (the manifold contact
        Xenia recorded: pos (-2267.3943, 1177.0439, -411.6295) n (0,1,0) d 0.0999023).

    python gen_cases.py <set> <out.script> [--n N] [--seed S] [--bodies K]
"""
import argparse
import json
import math
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))  # tools/golden.py
import golden  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
g = golden.GoldenDump("sound_gameplay")
snap = None   # loaded in main()

DISP = 0x022E6D00
FILTER_IF = 0x0236F0CC
COLL_INPUT = 0x0236E0C0
CONFIG = 0x0236E098
PHANTOM = 0x022E1A40
CD_A = PHANTOM + 0x14
PH_XFORM = 0x022E1AB0
CAPSULE_TYPE = 8
TERRAIN_BODY = 0x022ADF40

VT = 0x70000000
C_CAST = 0x70000100
C_START = 0x70000140
LCI = 0x70000200
CI = 0x70000300
AABB = 0x70000400
ARR = 0x70000500
HITS = 0x70010000
VA_COLLECT = 0x8290F500
VA_TRAP = 0x8290F504
FLT_MAX = 3.4028234663852886e38


def u32(a):
    return g.u32be(a)


def f32(a):
    return struct.unpack(">f", g.read(a, 4))[0]


def fb(*xs):
    return b"".join(struct.pack(">f", x) for x in xs)


def ub(*xs):
    return b"".join(struct.pack(">I", x & 0xFFFFFFFF) for x in xs)


def W(va, data):
    return "W %08X %s" % (va, data.hex().upper())


def shape_type(sa):
    return TYPES[sa]


TYPES = {}


def load_types(path):
    cur = None
    for l in open(path):
        l = l.strip()
        if l.startswith("CASE type"):
            cur = int(l.split()[2], 16)
        elif l.startswith("r3=") and cur is not None:
            TYPES[cur] = int(l[3:], 16)


def gen_types():
    shapes = {}
    for b in snap["bodies"]:
        if b.get("layer") in (6, 7) and b["shape"].get("addr"):
            shapes[int(b["shape"]["addr"], 16)] = b["shape"]["cls"]
            m = b["shape"].get("mopp")
            if m:
                shapes[int(m["child"], 16)] = m["child_cls"]
    shapes[0x0236E150] = "capsule"
    lines = []
    for sa in sorted(shapes):
        lines += ["CASE type %08X %s" % (sa, shapes[sa]), "CALL %08X r3=%X" % (u32(u32(sa) + 8), sa), "PR r3", "END"]
    return lines


def entry(type_a, type_b, slot):
    idx = g.read(DISP + 400 + 32 * type_a + type_b, 1)[0]
    return u32(DISP + 0x990 + 20 * idx + slot)


def vtable_lines():
    words = [VA_COLLECT if i == 1 else VA_TRAP + 4 * i for i in range(8)]
    return W(VT, ub(*words))


TMEM = 0x70000800


def tmem_lines():
    """fake hkThreadMemory for TLS slot 3 (dword_8297945C): +32 cur, +40 base marker, +44 end.
    vtable = the trap vtable (allocate/deallocate fallbacks log VCALL 3 / VCALL 4)."""
    # +16 m_memory = the game's real hkMemory (0x00B70058, havok_snapshot globals.hk_memory);
    # +48 = 0 -> no per-thread free lists: every alloc/free goes straight to the real pool.
    words = [VT, 0, 0, 0, 0x00B70058, 0, 0, 0, 0x70100000, 0, 0x700FFFF0, 0x70800000, 0]
    return [W(TMEM, ub(*words)), "TLS 3 %X" % TMEM]


def collector_lines(mode_cast, mode_start):
    return tmem_lines() + [vtable_lines(), W(C_CAST, ub(VT) + fb(FLT_MAX) + ub(mode_cast, 0)),
            W(C_START, ub(VT) + fb(FLT_MAX) + ub(mode_start, 0))]


def rot_from_axis_angle(ax, ang):
    x, y, z = ax
    n = math.sqrt(x * x + y * y + z * z) or 1.0
    x, y, z = x / n, y / n, z / n
    c, s = math.cos(ang), math.sin(ang)
    t = 1 - c
    # rows
    return [[t * x * x + c, t * x * y - s * z, t * x * z + s * y],
            [t * x * y + s * z, t * y * y + c, t * y * z - s * x],
            [t * x * z - s * y, t * y * z + s * x, t * z * z + c]]


GOLD_XFORM = g.read(PH_XFORM, 64)


def xform_bytes(rot_cols, pos, w=0.0):
    """hkTransform: rotation as 3 column vectors (hkMatrix3 column-major), then translation."""
    out = b""
    for col in range(3):
        out += fb(rot_cols[0][col], rot_cols[1][col], rot_cols[2][col], 0.0)
    return out + fb(pos[0], pos[1], pos[2], w)


def golden_rot_rows():
    v = struct.unpack(">16f", GOLD_XFORM)
    cols = [v[0:3], v[4:7], v[8:11]]
    return [[cols[c][r] for c in range(3)] for r in range(3)]


def layer7_bodies():
    out = []
    for b in snap["bodies"]:
        if b.get("layer") == 7 and b["shape"].get("addr") and b.get("aabb"):
            out.append(b)
    return out


def rnd_rot(rng):
    k = rng.random()
    if k < 0.5:
        return golden_rot_rows()
    if k < 0.7:
        return [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
    return rot_from_axis_angle((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)), rng.uniform(-math.pi, math.pi))


def gen_mq(rng, n_per):
    lines = [vtable_lines()]
    moppshapes = {}
    for b in layer7_bodies():
        m = b["shape"].get("mopp")
        if m:
            moppshapes[int(b["shape"]["addr"], 16)] = m
    for sa, m in sorted(moppshapes.items()):
        off = m["offset"][:3]
        scale = m["offset"][3]
        ext = 16777216.0 / scale
        vt = u32(sa)
        qfn = u32(vt + 36)
        boxes = []
        boxes.append(([o - 1 for o in off], [o + ext + 1 for o in off]))            # everything
        boxes.append(([o + ext * 0.5 for o in off], [o + ext * 0.5 for o in off]))  # degenerate point
        for k in range(n_per):
            kind = rng.random()
            if kind < 0.55:
                c = [o + rng.uniform(-0.1, 1.1) * ext for o in off]
                h = [math.exp(rng.uniform(math.log(0.01), math.log(ext * 0.5))) for _ in range(3)]
                boxes.append(([c[i] - h[i] for i in range(3)], [c[i] + h[i] for i in range(3)]))
            elif kind < 0.75:   # quantization-boundary coordinates: offset + n/scale*2^k exactly
                q = [o + (rng.randrange(0, 1 << 24) & ~0xFFFF) / scale for o in off]
                h = [(rng.randrange(1, 64) << 16) / scale for _ in range(3)]
                boxes.append((q, [q[i] + h[i] for i in range(3)]))
            elif kind < 0.9:    # thin slab spanning one axis fully (wall-like query)
                ax = rng.randrange(3)
                c = [o + rng.uniform(0, 1) * ext for o in off]
                lo = [c[i] - 0.3 for i in range(3)]
                hi = [c[i] + 0.3 for i in range(3)]
                lo[ax] = off[ax] - 1
                hi[ax] = off[ax] + ext + 1
                boxes.append((lo, hi))
            else:               # grazing the extent boundary
                ax = rng.randrange(3)
                c = [o + rng.uniform(0, 1) * ext for o in off]
                lo = [c[i] - 1 for i in range(3)]
                hi = [c[i] + 1 for i in range(3)]
                side = rng.random() < 0.5
                edge = off[ax] + (ext if side else 0.0)
                lo[ax], hi[ax] = (edge, edge + 0.5) if side else (edge - 0.5, edge)
                boxes.append((lo, hi))
        for i, (lo, hi) in enumerate(boxes):
            lines += ["CASE mq %08X %d" % (sa, i),
                      W(AABB, fb(lo[0], lo[1], lo[2], 0.0, hi[0], hi[1], hi[2], 0.0)),
                      W(ARR, ub(HITS, 0, 0x80000000 | 0x2000)),
                      "CALL %08X r3=%X r4=%X r5=%X" % (qfn, sa, AABB, ARR),
                      "DARR %08X 4" % ARR, "END"]
    return lines


def cast_case(name, body_addr, rot, start, end, tol, mep, mode_cast, mode_start, w=0.07):
    shp = u32(body_addr + 0x14)
    tb = TYPES[shp]
    fn = entry(CAPSULE_TYPE, tb, 12)
    path = [end[i] - start[i] for i in range(3)]
    plen = math.sqrt(sum(p * p for p in path))
    plen = struct.unpack(">f", struct.pack(">f", plen))[0]
    lci = ub(DISP, FILTER_IF) + fb(tol) + ub(0) + fb(path[0], path[1], path[2], 0.0) + fb(mep, plen) + ub(CONFIG, 0)
    return ["CASE %s" % name, W(PH_XFORM, xform_bytes(rot, start, w))] + collector_lines(mode_cast, mode_start) + [
        W(LCI, lci),
        "CALL %08X r3=%X r4=%X r5=%X r6=%X r7=%X" % (fn, CD_A, body_addr + 0x14, LCI, C_CAST, C_START),
        "LOG", "D %08X 10" % C_CAST, "D %08X 10" % C_START, "END"]


def closest_case(name, body_addr, rot, pos, tol, mode, w=0.07):
    shp = u32(body_addr + 0x14)
    tb = TYPES[shp]
    fn = entry(CAPSULE_TYPE, tb, 8)
    ci = g.read(COLL_INPUT, 48)
    ci = ci[:8] + fb(tol) + ci[12:]
    return ["CASE %s" % name, W(PH_XFORM, xform_bytes(rot, pos, w))] + collector_lines(mode, 0) + [
        W(CI, ci),
        "CALL %08X r3=%X r4=%X r5=%X r6=%X" % (fn, CD_A, body_addr + 0x14, CI, C_CAST),
        "LOG", "D %08X 10" % C_CAST, "END"]


def gen_lc(rng, n_per, bodies_limit):
    lines = [vtable_lines()]
    bodies = layer7_bodies()
    rng.shuffle(bodies)
    if bodies_limit:
        bodies = bodies[:bodies_limit]
    for b in bodies:
        ba = int(b["addr"], 16)
        lo, hi = b["aabb"]
        ctr = [(lo[i] + hi[i]) * 0.5 for i in range(3)]
        half = [(hi[i] - lo[i]) * 0.5 for i in range(3)]
        rad = math.sqrt(sum(h * h for h in half)) + 2.0
        for k in range(n_per):
            kind = rng.random()
            end = [lo[i] + rng.uniform(-0.05, 1.05) * (hi[i] - lo[i]) for i in range(3)]
            if kind < 0.6:      # from outside the AABB into it
                d = [rng.gauss(0, 1) for _ in range(3)]
                n = math.sqrt(sum(x * x for x in d)) or 1
                start = [ctr[i] + d[i] / n * rad for i in range(3)]
            elif kind < 0.85:   # short hop near a face (flying along a wall)
                ax = rng.randrange(3)
                end[ax] = lo[ax] - 0.35 if rng.random() < 0.5 else hi[ax] + 0.35
                start = [end[i] + rng.uniform(-3, 3) for i in range(3)]
            else:               # vertical drop onto / through the roof
                end[1] = hi[1] - rng.uniform(0, 1.0)
                start = [end[0], hi[1] + rng.uniform(0.5, 5), end[2]]
            tol = rng.choice([0.1, 0.1, 0.02, 0.5])
            mep = rng.choice([1.1920929e-07, 0.01])
            lines += cast_case("lc %08X %d" % (ba, k), ba, rnd_rot(rng), start, end, tol, mep,
                               rng.randrange(2), rng.randrange(2))
    return lines


def gen_cp(rng, n_per, bodies_limit):
    lines = [vtable_lines()]
    bodies = layer7_bodies()
    rng.shuffle(bodies)
    if bodies_limit:
        bodies = bodies[:bodies_limit]
    for b in bodies:
        ba = int(b["addr"], 16)
        lo, hi = b["aabb"]
        for k in range(n_per):
            pos = [lo[i] + rng.uniform(-0.03, 1.03) * (hi[i] - lo[i]) for i in range(3)]
            if rng.random() < 0.5:   # snap to a face so the capsule straddles the wall
                ax = rng.randrange(3)
                pos[ax] = (lo[ax] if rng.random() < 0.5 else hi[ax]) + rng.uniform(-0.5, 0.5)
            tol = rng.choice([0.1, 0.5, 2.0])
            lines += closest_case("cp %08X %d" % (ba, k), ba, rnd_rot(rng), pos, tol, rng.randrange(2))
    return lines


def gen_gold():
    lines = [vtable_lines()]
    v = struct.unpack(">16f", GOLD_XFORM)
    pos = v[12:15]
    rot = golden_rot_rows()
    ci_tol = f32(COLL_INPUT + 8)
    # (1) closest points at the untouched golden transform, world tolerance, both collector modes
    for mode in (0, 1):
        lines += ["CASE gold closest mode%d" % mode] + collector_lines(mode, 0) + [
            W(CI, g.read(COLL_INPUT, 48)),
            "CALL %08X r3=%X r4=%X r5=%X r6=%X" % (entry(CAPSULE_TYPE, TYPES[u32(TERRAIN_BODY + 0x14)], 8),
                                                   CD_A, TERRAIN_BODY + 0x14, CI, C_CAST),
            "LOG", "D %08X 10" % C_CAST, "END"]
    # (2) linear casts from the golden position along small idle paths (gravity step, zero-ish)
    for i, path in enumerate([(0, -0.0217778, 0), (0, -0.001, 0), (0.01, 0, 0), (0, -0.5, 0)]):
        end = [pos[j] + path[j] for j in range(3)]
        lines += cast_case("gold cast %d" % i, TERRAIN_BODY, rot, list(pos), end, ci_tol, 1.1920929e-07, 0, 0, w=v[15])
    return lines


AABBS_PATH = [None]
IDENT = 0x70000600
RAYIN = 0x70000700
RAYOUT = 0x70000780
RETB = 0x700007F0


def unique_l7_shapes():
    out = {}
    for b in layer7_bodies():
        out[int(b["shape"]["addr"], 16)] = b["shape"]["cls"]
    return out


def gen_aabb():
    """getAabb (vt+12) of every unique layer-7 building shape under the identity transform,
    tolerance 0 and 0.1 - also the pre-pass that gives the ray set its local extents."""
    lines = []
    ident = fb(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0)
    for sa, cls in sorted(unique_l7_shapes().items()):
        fn = u32(u32(sa) + 12)
        for tol in (0.0, 0.1):
            lines += ["CASE aabb %08X %s %g" % (sa, cls, tol), W(IDENT, ident),
                      "CALL %08X r3=%X r4=%X r6=%X f1=%016X" % (fn, sa, IDENT, AABB, struct.unpack(">Q", struct.pack(">d", tol))[0]),
                      "D %08X 20" % AABB, "END"]
    return lines


def load_aabbs(path):
    res, cur = {}, None
    for l in open(path):
        l = l.strip()
        if l.startswith("CASE aabb") and l.endswith(" 0"):
            cur = int(l.split()[2], 16)
        elif l.startswith("CASE"):
            cur = None
        elif l.startswith("D ") and cur is not None:
            v = struct.unpack(">8f", bytes.fromhex(l.split()[2]))
            res[cur] = (v[0:3], v[4:7])
    return res


def gen_ray(rng, n_per):
    """castRay (vt+20: hkBool* ret r3, this r4, hkShapeRayCastInput r5, hkShapeRayCastOutput r6) in
    shape-local space: rays through / around / grazing each building."""
    aabbs = load_aabbs(AABBS_PATH[0])
    lines = []
    for sa, cls in sorted(unique_l7_shapes().items()):
        if sa not in aabbs:
            continue
        lo, hi = aabbs[sa]
        fn = u32(u32(sa) + 20)
        ctr = [(lo[i] + hi[i]) * 0.5 for i in range(3)]
        rad = math.sqrt(sum(((hi[i] - lo[i]) * 0.5) ** 2 for i in range(3))) + 1.0
        for k in range(n_per):
            kind = rng.random()
            tgt = [lo[i] + rng.uniform(-0.05, 1.05) * (hi[i] - lo[i]) for i in range(3)]
            d = [rng.gauss(0, 1) for _ in range(3)]
            nn = math.sqrt(sum(x * x for x in d)) or 1
            if kind < 0.6:      # long ray through a random interior point, both ends outside
                frm = [tgt[i] + d[i] / nn * rad * 1.2 for i in range(3)]
                to = [tgt[i] - d[i] / nn * rad * 1.2 for i in range(3)]
            elif kind < 0.8:    # short ray ending near a face
                ax = rng.randrange(3)
                tgt[ax] = lo[ax] if rng.random() < 0.5 else hi[ax]
                frm = [tgt[i] + d[i] / nn * rng.uniform(0.05, 3) for i in range(3)]
                to = [tgt[i] - d[i] / nn * rng.uniform(0.0, 0.5) for i in range(3)]
            else:               # axis-aligned ray (degenerate directions) along a face
                ax = rng.randrange(3)
                frm = list(tgt); to = list(tgt)
                frm[ax] = lo[ax] - 1.0; to[ax] = hi[ax] + 1.0
            out0 = fb(0, 0, 0, 0, 1.0) + ub(0xFFFFFFFF) + ub(*([0xFFFFFFFF] * 8)) + ub(0, 0)
            lines += ["CASE ray %08X %s %d" % (sa, cls, k),
                      W(RAYIN, fb(frm[0], frm[1], frm[2], 0, to[0], to[1], to[2], 0) + ub(0, 0)),
                      W(RAYOUT, out0), W(RETB, ub(0)),
                      "CALL %08X r3=%X r4=%X r5=%X r6=%X" % (fn, RETB, sa, RAYIN, RAYOUT),
                      "D %08X 4" % RETB, "D %08X 40" % RAYOUT, "END"]
    return lines


PROXY = 0x0236E3E0
INTEGRATE = 0x825EC5A8
STEP = 0x70000A00
GRAV = 0x70000A20
PC_CAST = 0x70000B00
PC_START = 0x70000C00


def integrate_case(name, pos, vel, dt=1.0 / 30.0, rot=None):
    lines = ["CASE %s" % name] + tmem_lines()
    if pos is not None:
        lines.append(W(PH_XFORM, xform_bytes(rot or golden_rot_rows(), pos, struct.unpack(">16f", GOLD_XFORM)[15])))
    if vel is not None:
        lines.append(W(PROXY + 0x10, fb(vel[0], vel[1], vel[2], 0.0)))
    lines += [W(STEP, fb(100.0, 100.0 + dt, dt, 1.0 / dt)),
              W(GRAV, fb(0.0, -19.6, 0.0, 0.0)),
              W(PC_CAST, ub(0x82009004) + fb(FLT_MAX) + ub(0, 0, 0x70020000, 0, 0x80000100)),
              W(PC_START, ub(0x82009004) + fb(FLT_MAX) + ub(0, 0, 0x70030000, 0, 0x80000100)),
              "CALL %08X r3=%X r4=%X r5=%X r6=%X r7=%X" % (INTEGRATE, PROXY, STEP, GRAV, PC_CAST, PC_START),
              "LOG", "CS",
              "D %08X C0" % PROXY, "D %08X 40" % PH_XFORM,
              "DARR %08X 30" % (PROXY + 0x74), "DARR %08X 30" % (PROXY + 0x80),
              "DARR %08X 30" % (PC_CAST + 16), "DARR %08X 30" % (PC_START + 16), "END"]
    return lines


def gen_ig(rng, n_per, bodies_limit):
    lines = []
    v = struct.unpack(">16f", GOLD_XFORM)
    gpos = list(v[12:15])
    lines += integrate_case("ig golden asis", None, None)
    for i, vel in enumerate([(0, 0, 0), (5, 0, 0), (0, -30, 0), (20, 5, -20), (0, 60, 0)]):
        lines += integrate_case("ig golden vel%d" % i, None, vel)
    bodies = layer7_bodies()
    rng.shuffle(bodies)
    if bodies_limit:
        bodies = bodies[:bodies_limit]
    for b in bodies:
        lo, hi = b["aabb"]
        ba = int(b["addr"], 16)
        for k in range(n_per):
            ax = rng.choice([0, 2, 0, 2, 1])
            side = rng.random() < 0.5
            p = [lo[i] + rng.uniform(0.2, 0.8) * (hi[i] - lo[i]) for i in range(3)]
            off = rng.choice([1.5, 0.6, 0.31, 0.2])       # 0.2 < capsule radius 0.3: starts penetrating
            p[ax] = (lo[ax] - off) if side else (hi[ax] + off)
            if ax == 1 and not side:
                p[1] = hi[1] + 0.83 + off                 # above the roof (capsule bottom is -0.83-0.3)
            speed = rng.choice([3.0, 10.0, 25.0, 60.0])
            d = [0.0, 0.0, 0.0]
            d[ax] = 1.0 if side else -1.0
            d = [d[i] + rng.uniform(-0.4, 0.4) for i in range(3)]
            n = math.sqrt(sum(x * x for x in d))
            vel = [d[i] / n * speed for i in range(3)]
            lines += integrate_case("ig %08X %d" % (ba, k), p, vel, rot=rnd_rot(rng))
    return lines


CT_TRI = 0x70000D00
CT_CAPS = 0x70000D40
CT_CACHE = 0x70000D80
CT_OUT = 0x70000DC0


def d64(x):
    return "%016X" % struct.unpack(">Q", struct.pack(">d", struct.unpack(">f", struct.pack(">f", x))[0]))[0]


def gen_ct(rng, n):
    """D2: hkCapsuleTriangleClosestPoints 0x825C3C08 directly (pure VMX128 geometry): cache from
    hkTriangleDistanceCacheSetup 0x825C2F50(tri r3, cache r4), then (caps r3, capR f1, tri r5,
    triR f2, cache r7, tolerance f3, r9=0, out r10 = hkContactPoint[2])."""
    lines = []
    for k in range(n):
        sc = math.exp(rng.uniform(math.log(0.1), math.log(20)))
        kind = rng.random()
        A = [0.0, 0.0, 0.0]
        B = [sc * rng.uniform(0.2, 1), 0.0, 0.0]
        C = [sc * rng.uniform(-0.5, 1), sc * rng.uniform(0.2, 1), 0.0]
        if kind < 0.05:                                  # sliver / degenerate triangle
            C = [B[0] * 0.5, sc * 1e-4, 0.0]
        rot = rot_from_axis_angle((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)), rng.uniform(-3.14, 3.14))
        off = [rng.uniform(-50, 50) for _ in range(3)]
        def xf(p):
            return [sum(rot[r][c] * p[c] for c in range(3)) + off[r] for r in range(3)]
        tri = [xf(A), xf(B), xf(C)]
        cen = [(tri[0][i] + tri[1][i] + tri[2][i]) / 3 for i in range(3)]
        n_ = [sum(rot[r][c] * v for c, v in enumerate((0, 0, 1))) for r in range(3)]
        r = rng.choice([0.3, 0.3, 0.05, 1.0])
        h = rng.choice([1.23, 0.0, 2.5])                 # capsule segment length (0 = sphere)
        k2 = rng.random()
        pl = [tri[0][i] + rng.uniform(-0.2, 1.1) * (tri[1][i] - tri[0][i]) + rng.uniform(-0.2, 1.1) * (tri[2][i] - tri[0][i]) for i in range(3)]
        dist = rng.choice([0.0, r * 0.5, r, r + 0.05, r + 1.0, -r * 0.5])
        if k2 < 0.3:     # segment parallel to the plane
            d = [rng.gauss(0, 1) for _ in range(3)]
            dn = sum(d[i] * n_[i] for i in range(3))
            d = [d[i] - dn * n_[i] for i in range(3)]
        elif k2 < 0.6:   # perpendicular
            d = list(n_)
        elif k2 < 0.75:  # parallel to an edge
            d = [tri[1][i] - tri[0][i] for i in range(3)]
        else:
            d = [rng.gauss(0, 1) for _ in range(3)]
        dl = math.sqrt(sum(x * x for x in d)) or 1
        d = [x / dl for x in d]
        p0 = [pl[i] + n_[i] * dist for i in range(3)]
        p1 = [p0[i] + d[i] * h for i in range(3)]
        tol = rng.choice([0.1, 0.5, 2.0])
        trir = rng.choice([0.0, 0.0, 0.05])
        lines += ["CASE ct %d" % k,
                  W(CT_TRI, fb(*tri[0], 0, *tri[1], 0, *tri[2], 0)),
                  W(CT_CAPS, fb(*p0, 0, *p1, 0)),
                  "CALL 825C2F50 r3=%X r4=%X" % (CT_TRI, CT_CACHE),
                  "D %08X 20" % CT_CACHE,
                  "CALL 825C3C08 r3=%X r5=%X r7=%X r9=0 r10=%X f1=%s f2=%s f3=%s" % (CT_CAPS, CT_TRI, CT_CACHE, CT_OUT, d64(r), d64(trir), d64(tol)),
                  "PR r3", "D %08X 40" % CT_OUT, "END"]
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("set")
    ap.add_argument("out")
    ap.add_argument("--n", type=int, default=8)
    ap.add_argument("--seed", type=int, default=463)
    ap.add_argument("--bodies", type=int, default=0)
    work_out = os.path.join(ROOT, "archive", "golden_call", "out")
    ap.add_argument("--snapshot", default=os.path.join(ROOT, "archive", "v1_invest", "collision", "snapshots", "xenia_sound_gameplay.json"),
                    help="tools/havok_snapshot.py JSON of the SAME golden dump")
    ap.add_argument("--types", default=os.path.join(work_out, "types.recomp.out"), help="output of the `types` set")
    ap.add_argument("--aabbs", default=os.path.join(work_out, "aabb.recomp.out"), help="output of the `aabb` set")
    a = ap.parse_args()
    global snap
    snap = json.load(open(a.snapshot))
    AABBS_PATH[0] = a.aabbs
    if a.set != "types":
        load_types(a.types)
    rng = random.Random(a.seed)
    if a.set == "types":
        lines = gen_types()
    elif a.set == "mq":
        lines = gen_mq(rng, a.n)
    elif a.set == "lc":
        lines = gen_lc(rng, a.n, a.bodies)
    elif a.set == "cp":
        lines = gen_cp(rng, a.n, a.bodies)
    elif a.set == "aabb":
        lines = gen_aabb()
    elif a.set == "ray":
        lines = gen_ray(rng, a.n)
    elif a.set == "ig":
        lines = gen_ig(rng, a.n, a.bodies)
    elif a.set == "ct":
        lines = gen_ct(rng, a.n)
    elif a.set == "gold":
        lines = gen_gold()
    else:
        raise SystemExit("unknown set")
    open(a.out, "w").write("\n".join(lines) + "\n")
    print(a.set, sum(1 for l in lines if l.startswith("CASE")), "cases ->", a.out)


if __name__ == "__main__":
    main()
