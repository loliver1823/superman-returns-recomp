#!/usr/bin/env python3
"""gen_mover_cases.py - golden_call scripts for Superman's GAME-SIDE mover (collision map iter-463):
the actor tick phases that move him, run on a golden (or our-dump) memory image by BOTH the recomp
harness and the clean-room interpreter (scripts/difftest/golden_call, TOOLKIT 1.3b).

Call path under test (IDA, collision map): actor vt+4 = sub_821D4CA0(actor r3, phase r5, dt f1)
  phase 0 -> sub_821BE6C8: FSM handler (+0x84) vt[1], then the loco slot (+0x60) update
             (LOCO / FLY LocoFly_Update_sub_82224A90 / ANIM): writes VELOCITY only
  phase 1 -> Entity_TickProxyTransformInterp_sub_821B17C0: anim player, root motion (velocity)
  phase 2 -> PhysCharProxyObj vt+8 = MoveTowardIdealPos_sub_821C0C58: the gates
             (+0x0F bit2 active, off-camera skip 0x821C0DD0 via the NDC test sub_82175378 and the
             golden camera, keyframed override 0x821C1124) and hkCharacterProxy::integrate.

Sets (--image golden = xenia:sound_gameplay | midtown = our Midtown dump, Superman IN FLIGHT):
  probe : camera position (Cam_GetActivePosition 0x821624D8), the NDC test of Superman's own
          position, one phase-2 tick and one full 0/1/2 tick as-is.
  mv    : phase 2 only, T ticks: Superman teleported (entity pos, the authoritative input) next to
          a layer-7 building with a velocity into its face. Variants: 'asis' (image state) and, on
          the golden, 'flt' (flight proxy state as in our Midtown dump: +0x0F=0x06 no gravity,
          +0x83=1, +0x7C=0).
  gate  : the designed bypasses, to prove the branch decisions and their effect match: +0x84=1
          (off-camera skip armed) at the camera / 100 m away / 10 m around the camera in 8
          directions (the NDC test decides), direct NDC calls on the same points, and the
          keyframed override (+0x0E=0x12) flying into a wall.
  path  : castRay of every segment a PASS-THROUGH case travelled (--src script recomp.out) against
          its target building: hit = geometry was crossed without contact (a real miss).
  fly   : the flight tick T times into a wall: loco slot update sub_821EA5B0 (FLY mover), phase 1,
          phase 2 (--full-fsm: the whole phase 0 incl. the FSM handler/Lua instead). golden: the game's own
          LocoController_ApplyMoveDef_sub_8221C1E0(slot, SupermanFlight=8) switches him to the FLY
          loco first; midtown: already flying.

    python gen_mover_cases.py <set> <out.script> [--image golden|midtown] [--bodies K] [--ticks T]
Outputs are classified by classify_mover.py (same directory).
"""
import argparse
import json
import math
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import havok_snapshot as hs  # noqa: E402
import superman_mover_state as sms  # noqa: E402

SNAPDIR = os.path.join(ROOT, "archive", "v1_invest", "collision", "snapshots")
IMAGES = {
    "golden": {"spec": "xenia:sound_gameplay", "actor": None,
               "snapshot": os.path.join(SNAPDIR, "xenia_sound_gameplay.json")},
    "midtown": {"spec": "ours:" + os.path.join(ROOT, "archive", "v1_invest", "collision", "ours_midtown.dmp"),
                "actor": 0x841C9ED0, "snapshot": os.path.join(SNAPDIR, "ours_midtown.json")},
}

TICK = 0x821D4CA0            # actor vt+4 phase dispatcher
LOCO_SLOT_UPDATE = 0x821EA5B0  # phase 0 tail: loco slot update (slot r3, dt f1) -> the loco's vt[2]
APPLY_MOVEDEF = 0x8221C1E0   # LocoController_ApplyMoveDef(slot r3, idx r4)
CAM_POS = 0x821624D8         # Cam_GetActivePosition(director r3, out r4)
NDC_TEST = 0x82175378        # (director r3, point r4) -> r3 != 0 inside the frustum
G_CAMDIR = 0x8296D86C
G_TLS_HKMEM = 0x8297945C     # TLS index of hkThreadMemory
MOVEDEF_FLIGHT = 8           # "SupermanFlight" (0x8296DF58 table, superman_mover_state)
DT = 1.0 / 30.0

VT = 0x70000000
TMEM = 0x70000800
OUT_CAM = 0x70000E00
PT = 0x70000E40
VA_COLLECT = 0x8290F500
VA_TRAP = 0x8290F504


def fb(*xs):
    return b"".join(struct.pack(">f", x) for x in xs)


def ub(*xs):
    return b"".join(struct.pack(">I", x & 0xFFFFFFFF) for x in xs)


def W(va, data):
    return "W %08X %s" % (va, data.hex().upper())


def d64(x):
    return "%016X" % struct.unpack(">Q", struct.pack(">d", struct.unpack(">f", struct.pack(">f", x))[0]))[0]


class Ctx:
    def __init__(self, name):
        cfg = IMAGES[name]
        self.name = name
        self.img = hs.open_image(cfg["spec"])
        st = sms.decode(self.img, cfg["actor"])
        self.st = st
        self.actor = int(st["actor"], 16)
        self.po = int(st["prxy_obj"], 16)
        self.proxy = int(st["hkCharacterProxy"], 16)
        self.slot = int(st["loco_slot"], 16)
        self.snap = json.load(open(cfg["snapshot"]))
        self.hkmem = int(self.snap["globals"]["hk_memory"], 16)
        self.tls_idx = self.img.u32(G_TLS_HKMEM)
        self.camdir = self.img.u32(G_CAMDIR)
        self.pos = struct.unpack(">4f", self.img.read(self.actor + 0x20, 16))
        self.vel = struct.unpack(">4f", self.img.read(self.proxy + 0x10, 16))

    def l7(self):
        return [b for b in self.snap["bodies"] if b.get("layer") == 7 and b.get("aabb") and b["shape"].get("addr")]

    def nearest_l7(self, k, min_h=20.0):
        p = self.pos
        rows = []
        for b in self.l7():
            lo, hi = b["aabb"]
            if hi[1] - lo[1] < min_h:
                continue
            d = math.sqrt(sum(max(lo[i] - p[i], 0, p[i] - hi[i]) ** 2 for i in range(3)))
            rows.append((d, b))
        rows.sort(key=lambda r: r[0])
        out = [b for _, b in rows[:k]]
        # plus the nearest MOPP buildings (about a third of layer 7; the nearest few are list shapes)
        mopp = [b for _, b in rows if b["shape"]["cls"] == "hkMoppBvTreeShape" and b not in out]
        return out + mopp[:max(1, k // 3)]


def header(c):
    """harness TLS hkThreadMemory (as in gen_cases.tmem_lines) + trap vtable."""
    words = [VT, 0, 0, 0, c.hkmem, 0, 0, 0, 0x70100000, 0, 0x700FFFF0, 0x70800000, 0]
    vt = [VA_COLLECT if i == 1 else VA_TRAP + 4 * i for i in range(8)]
    return [W(VT, ub(*vt)), W(TMEM, ub(*words)), "TLS %X %X" % (c.tls_idx, TMEM)]


def dumps(c):
    return ["D %08X 10" % (c.actor + 0x20), "D %08X 10" % (c.proxy + 0x10),
            "D %08X 10" % (c.po + 0x7C), "D %08X 4" % (c.po + 0x0C),
            "DARR %08X 30" % (c.proxy + 0x74)]


def tick(c, phases, dt=DT):
    out = []
    for ph in phases:
        out.append("CALL %08X r3=%X r5=%X f1=%s" % (TICK, c.actor, ph, d64(dt)))
    return out + dumps(c)


def start_case(c, name, pos=None, vel=None, flags=()):
    lines = ["CASE %s" % name] + header(c)
    if pos is not None:
        lines.append(W(c.actor + 0x20, fb(pos[0], pos[1], pos[2], c.pos[3])))
    if vel is not None:
        lines.append(W(c.proxy + 0x10, fb(vel[0], vel[1], vel[2], 0.0)))
    for off, data in flags:
        lines.append(W(c.po + off, data))
    return lines


FLT_FLAGS = [(0x0F, bytes([0x06])), (0x83, bytes([1])), (0x7C, bytes([0]))]


GAP = 1.0          # start: capsule centre 1 m in front of the AABB face
DEPTH = 10.0       # run long enough to end DEPTH m past the face if nothing stops him


def auto_ticks(speed, ticks):
    return ticks if ticks else max(2, min(90, int(math.ceil((GAP + DEPTH) / (speed * DT)))))


def wall_approach(rng, b, speed, ticks, side_pref=None):
    """a start point GAP m in front of one vertical AABB face and a velocity into it; free flight
    would carry him DEPTH m past the face within auto_ticks(speed) ticks."""
    lo, hi = b["aabb"]
    ax = rng.choice([0, 2]) if side_pref is None else side_pref[0]
    side = rng.random() < 0.5 if side_pref is None else side_pref[1]
    p = [lo[i] + rng.uniform(0.25, 0.75) * (hi[i] - lo[i]) for i in range(3)]
    p[1] = lo[1] + min(hi[1] - lo[1] - 4.0, rng.uniform(6.0, 40.0))
    gap = GAP
    p[ax] = (lo[ax] - gap) if side else (hi[ax] + gap)
    d = [0.0, 0.0, 0.0]
    d[ax] = 1.0 if side else -1.0
    d = [d[i] + (rng.uniform(-0.25, 0.25) if i != ax else 0.0) for i in range(3)]
    n = math.sqrt(sum(x * x for x in d))
    return p, [d[i] / n * speed for i in range(3)], ax, side


def gen_probe(c):
    lines = ["CASE probe camera"] + header(c) + [
        "CALL %08X r3=%X r4=%X" % (CAM_POS, c.camdir, OUT_CAM), "D %08X 10" % OUT_CAM,
        W(PT, fb(*c.pos)), "CALL %08X r3=%X r4=%X" % (NDC_TEST, c.camdir, PT), "PR r3", "END"]
    lines += start_case(c, "probe phase2 asis") + tick(c, [2]) + ["LOG", "CS", "END"]
    lines += start_case(c, "probe full asis") + tick(c, [0, 1, 2]) + ["LOG", "CS", "END"]
    return lines


SPEEDS = [10.0, 25.0, 60.0, 134.0, 250.0]   # 134 m/s ~ 300 mph (flight cap band)


def gen_mv(c, rng, nbodies, ticks):
    lines = []
    variants = ["asis"] + (["flt"] if c.name == "golden" else [])
    for b in c.nearest_l7(nbodies):
        for sp in SPEEDS:
            p, v, ax, side = wall_approach(rng, b, sp, ticks)
            for var in variants:
                name = "mv %s %s %s v%g ax%d%s" % (var, b["addr"], b["shape"]["cls"], sp, ax, "+" if side else "-")
                lines += start_case(c, name, p, v, FLT_FLAGS if var == "flt" else ())
                for _ in range(auto_ticks(sp, ticks)):
                    lines += tick(c, [2])
                lines += ["LOG", "CS", "END"]
    return lines


def gen_gate(c, rng, ticks):
    lines = ["CASE gate campos"] + header(c) + [
        "CALL %08X r3=%X r4=%X" % (CAM_POS, c.camdir, OUT_CAM), "D %08X 10" % OUT_CAM, "END"]
    campos = None
    pts = []
    # candidate points: Superman's own position, 100 m up, 10 m around him (the camera orbits
    # him at ~4-10 m, so these straddle the frustum edges)
    base = c.pos[:3]
    pts.append(("self", list(base)))
    pts.append(("far100", [base[0], base[1] + 100.0, base[2]]))
    for k in range(8):
        a = k * math.pi / 4
        pts.append(("ring%d" % k, [base[0] + 10 * math.cos(a), base[1] + 1.0, base[2] + 10 * math.sin(a)]))
    for nm, p in pts:
        lines += ["CASE gate ndc %s" % nm] + header(c) + [
            W(PT, fb(p[0], p[1], p[2], 0.0)), "CALL %08X r3=%X r4=%X" % (NDC_TEST, c.camdir, PT), "PR r3", "END"]
        # armed off-camera skip (+0x84=1, +0x7C=1): one phase-2 tick with a sideways velocity
        lines += start_case(c, "gate skip %s" % nm, p, [5.0, 0.0, 0.0],
                            [(0x84, ub(1)), (0x7C, bytes([1]))]) + tick(c, [2]) + ["LOG", "CS", "END"]
    # keyframed override into walls (mode nibble 1): integrate runs, result discarded
    for b in c.nearest_l7(3):
        p, v, ax, side = wall_approach(rng, b, 25.0, ticks)
        lines += start_case(c, "gate kf %s ax%d%s" % (b["addr"], ax, "+" if side else "-"), p, v,
                            [(0x0E, bytes([0x12]))])
        for _ in range(auto_ticks(25.0, ticks)):
            lines += tick(c, [2])
        lines += ["LOG", "CS", "END"]
    return lines


def gen_fly(c, rng, nbodies, ticks, full_fsm=False):
    lines = []
    for b in c.nearest_l7(nbodies):
        for sp in [10.0, 60.0, 134.0]:
            p, v, ax, side = wall_approach(rng, b, sp, ticks)
            name = "fly %s %s v%g ax%d%s" % (b["addr"], b["shape"]["cls"], sp, ax, "+" if side else "-")
            lines += start_case(c, name)
            if c.name == "golden":
                lines += ["CALL %08X r3=%X r4=%X" % (APPLY_MOVEDEF, c.slot, MOVEDEF_FLIGHT), "PR r3",
                          "D %08X 8" % c.slot]
            lines += [W(c.actor + 0x20, fb(p[0], p[1], p[2], c.pos[3])), W(c.proxy + 0x10, fb(v[0], v[1], v[2], 0.0))]
            for _ in range(auto_ticks(sp, ticks)):
                if full_fsm:
                    lines += tick(c, [0, 1, 2])
                else:   # the FLY mover alone: loco slot update (phase 0 minus the FSM/Lua handler)
                    lines += ["CALL %08X r3=%X f1=%s" % (LOCO_SLOT_UPDATE, c.slot, d64(DT))] + tick(c, [1, 2])
            lines += ["D %08X 8" % c.slot, "LOG", "CS", "END"]
    return lines


RAYIN = 0x70000700
RAYOUT = 0x70000780
RETB = 0x700007F0


def gen_path(c, src_script, src_out, only_pass):
    """castRay (shape vt+20, shape space) of every per-tick segment an earlier case actually
    travelled, at the capsule's centre and +-0.8 m, against the TARGET building: a hit on a
    segment with no manifold contact would be a real collision miss; no hit = no geometry there."""
    import classify_mover as cm   # noqa: E402  (same directory)
    from classify import cases as read_cases  # noqa: E402
    meta = {"actor": c.actor, "prxy_obj": c.po, "proxy": c.proxy,
            "bodies": {b["addr"]: {"aabb": b["aabb"]} for b in c.l7()}}
    S = cm.script_cases(src_script)
    R, order = read_cases(src_out)
    lines = []
    for name in order:
        ticks, _ = cm.parse_out(R[name], meta)
        p0, v0 = cm.start_of(S.get(name, []), meta)
        kind, _info = cm.outcome(name, ticks, p0, v0, meta)
        if not kind or (only_pass and kind != "PASS-THROUGH"):
            continue
        tgt = next(x for x in name.split() if x.startswith("0x"))
        body = next(b for b in c.l7() if b["addr"] == tgt)
        sa = int(body["shape"]["addr"], 16)
        xf = struct.unpack(">16f", c.img.read(int(body["transform"], 16), 64))
        cols, t = [xf[0:3], xf[4:7], xf[8:11]], xf[12:15]
        fn = c.img.u32(c.img.u32(sa) + 20)

        def local(p):   # R^T (p - t), R stored as columns
            d = [p[i] - t[i] for i in range(3)]
            return [sum(cols[k][i] * d[i] for i in range(3)) for k in range(3)]
        pts = [list(p0)] + [list(tk["pos"]) for tk in ticks]
        for i in range(len(pts) - 1):
            for dy in (-0.8, 0.0, 0.8):
                a = local([pts[i][0], pts[i][1] + dy, pts[i][2]])
                b = local([pts[i + 1][0], pts[i + 1][1] + dy, pts[i + 1][2]])
                out0 = fb(0, 0, 0, 0, 1.0) + ub(0xFFFFFFFF) + ub(*([0xFFFFFFFF] * 8)) + ub(0, 0)
                lines += ["CASE path %s | tick%d dy%+.1f" % (name, i + 1, dy),
                          W(RAYIN, fb(a[0], a[1], a[2], 0, b[0], b[1], b[2], 0) + ub(0, 0)),
                          W(RAYOUT, out0), W(RETB, ub(0)),
                          "CALL %08X r3=%X r4=%X r5=%X r6=%X" % (fn, RETB, sa, RAYIN, RAYOUT),
                          "D %08X 4" % RETB, "D %08X 40" % RAYOUT, "END"]
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("set", choices=["probe", "mv", "gate", "fly", "path"])
    ap.add_argument("--src", nargs=2, metavar=("SCRIPT", "RECOMP_OUT"), help="path: the case set to re-trace")
    ap.add_argument("--all", action="store_true", help="path: every wall case, not only PASS-THROUGH")
    ap.add_argument("out")
    ap.add_argument("--image", default="golden", choices=sorted(IMAGES))
    ap.add_argument("--bodies", type=int, default=6)
    ap.add_argument("--ticks", type=int, default=0, help="ticks per case (0 = auto per speed)")
    ap.add_argument("--seed", type=int, default=463)
    ap.add_argument("--full-fsm", action="store_true", help="fly: run the whole phase 0 (FSM handler + Lua)")
    ap.add_argument("--meta", help="write the case metadata JSON (bodies, start, velocity) here")
    a = ap.parse_args()
    c = Ctx(a.image)
    rng = random.Random(a.seed)
    if a.set == "probe":
        lines = gen_probe(c)
    elif a.set == "mv":
        lines = gen_mv(c, rng, a.bodies, a.ticks)
    elif a.set == "gate":
        lines = gen_gate(c, rng, a.ticks)
    elif a.set == "path":
        sys.path.insert(0, HERE)
        sys.path.insert(0, os.path.join(HERE, "..", "golden_call"))
        lines = gen_path(c, a.src[0], a.src[1], not a.all)
    else:
        lines = gen_fly(c, rng, a.bodies, a.ticks, a.full_fsm)
    open(a.out, "w").write("\n".join(lines) + "\n")
    meta = {"image": a.image, "actor": c.actor, "prxy_obj": c.po, "proxy": c.proxy, "slot": c.slot,
            "camdir": c.camdir, "tls_idx": c.tls_idx, "pos": c.pos, "vel": c.vel, "state": c.st,
            "bodies": {b["addr"]: {"name": b.get("name"), "cls": b["shape"]["cls"], "aabb": b["aabb"]}
                       for b in c.l7()}}
    json.dump(meta, open(a.meta or os.path.splitext(a.out)[0] + ".meta.json", "w"), indent=0, default=str)
    print(a.set, a.image, sum(1 for l in lines if l.startswith("CASE")), "cases ->", a.out)


if __name__ == "__main__":
    main()
