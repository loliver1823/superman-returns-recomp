#!/usr/bin/env python3
"""Tests for tools/objstate.py (python tools/test_objstate.py).

Everything runs on a SYNTHETIC guest image built here from the layouts objstate.py cites,
so the suite needs no dump. Three layers:
  1. rule tests: each clause of the sub_822737D0 availability port, the dependency
     tokenizer (sub_8224DF00 + Str_AtoiLenient_sub_825F5380), the hashes, the lookups;
  2. image mutations: a corrupted / foreign image must show up as failed structural checks
     or as a changed verdict, never as a quiet plausible number;
  3. code mutants (MutantTest): each ported primitive is replaced by a plausible wrong
     version and the suite must fail for every mutant (mutation-validates the tests).
"""
import copy
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import objstate as O  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# the image builder uses frozen copies so a code mutant cannot also rebuild the image
_FNV1, _HASH31 = O.fnv1, O.hash31


def be32(v):
    return struct.pack(">I", v & 0xFFFFFFFF)


class World:
    """Builds a minimal but complete retail-shaped guest image."""

    HEAP = 0x00400000

    def __init__(self):
        self.img = O.SyntheticImage()
        self.brk = self.HEAP
        self.atoms = {}          # key -> string ptr
        self._strings = {}
        self.img.plant(0x82000000, b"MZ")
        self.img.plant(O.VT_TRIGGER_STATE, be32(0x82297AE0) + be32(0x822737D0) + be32(0x82273B40))
        self.img.plant(0x822737D0, be32(0x7D8802A6))
        self.img.plant(0x82224EE0, be32(0x7D8802A6))
        self.img.plant(O.F_ZERO_820013B8, struct.pack(">f", 0.0))
        self.img.plant(O.G_FSM_CURRENT, be32(0))
        # stat getter `lfd f1,8(r3); blr` and the trigger vt+36 getter (ACTR, GAME, else 0)
        self.stat_vt = self.alloc(64)
        self.stat_getter = self.alloc(16)
        self.img.plant(self.stat_getter, be32(0xC8230008) + be32(0x4E800020))
        self.img.plant(self.stat_vt + 40, be32(self.stat_getter))
        # every uncapped retail stat class: vt+20 (SetStatCap) = `blr` (0x8231C140)
        self.blr = self.alloc(4)
        self.img.plant(self.blr, be32(0x4E800020))
        self.img.plant(self.stat_vt + 20, be32(self.blr))
        # flow-FSM class fingerprint and the input-event table (sub_8229A568)
        self.img.plant(O.VT_FLOW_FSM, be32(0x8223FBC0) + be32(0x82293108))
        for i, (ident, nm) in enumerate(INPUT_EVENTS):
            self.img.plant(O.G_INPUT_EVENTS + 12 * i, be32(ident) + be32(self.cstr(nm)) + be32(_FNV1(nm)))
        self.img.plant(O.G_INPUT_EVENTS + 12 * len(INPUT_EVENTS), be32(0) + be32(0) + be32(0))
        self.img.plant(O.G_INPUT_EVENTS + 12 * (len(INPUT_EVENTS) + 1),
                       be32(0x44) + be32(self.cstr(AFTER_TERMINATOR)) + be32(_FNV1(AFTER_TERMINATOR)))
        self.actor_vt = self.alloc(64)
        self.actor_getter = self.alloc(48)
        code = (be32(0x3D604143) + be32(0x616B5452) + be32(0x7F045800) + be32(0x4D9A0020)
                + be32(0x3D604741) + be32(0x616B4D45) + be32(0x7F045800) + be32(0x4D9A0020)
                + be32(0x38600000) + be32(0x4E800020))
        self.img.plant(self.actor_getter, code)
        self.img.plant(self.actor_vt + 36, be32(self.actor_getter))
        self.img.plant(0x82912090, be32(self.cstr(b"OBJ|TRIGGERS")))
        self.img.plant(0x829120BC, be32(self.cstr(b"objectiveIndex")))

    # -- memory helpers --
    def alloc(self, n, align=16):
        self.brk = (self.brk + align - 1) & ~(align - 1)
        va = self.brk
        self.brk += max(n, 4)
        self.img.plant(va, b"\0" * max(n, 4))
        return va

    def cstr(self, s):
        if s in self._strings:
            return self._strings[s]
        va = self.alloc(len(s) + 1, 4)
        self.img.plant(va, s + b"\0")
        self._strings[s] = va
        return va

    def w32(self, va, v):
        self.img.plant(va, be32(v))

    def vec(self, items):
        arr = self.alloc(4 * len(items) + 4)
        for i, x in enumerate(items):
            self.w32(arr + 4 * i, x)
        v = self.alloc(12)
        self.w32(v, arr)
        self.w32(v + 4, arr + 4 * len(items))
        return v, arr

    # -- atoms (std::map: +0 right, +4 left, +16 key, +28 value) --
    def atom(self, s):
        k = _FNV1(s)
        self.atoms[k] = self.cstr(s)
        return k

    def finish_atoms(self):
        keys = sorted(self.atoms)

        def build(lo, hi):
            if lo >= hi:
                return 0
            mid = (lo + hi) // 2
            n = self.alloc(32)
            self.w32(n + 0, build(mid + 1, hi))
            self.w32(n + 4, build(lo, mid))
            self.w32(n + 16, keys[mid])
            self.w32(n + 28, self.atoms[keys[mid]])
            return n
        tree = self.alloc(16)
        self.w32(tree + 12, build(0, len(keys)))
        holder = self.alloc(4)
        self.w32(holder, tree)
        self.w32(O.G_ATOMS, holder)

    # -- DAS --
    def das(self, attrs, persistent=()):
        """attrs: name -> (type, value). Returns the handle."""
        nodes = []
        for name, (t, v) in attrs.items():
            k = self.atom(name)
            if t == 1:
                raw = struct.unpack(">I", struct.pack(">f", v))[0]
            elif t == 6:
                raw = self.atom(v) if v is not None else 0xDEAD0001
            else:
                raw = v & 0xFFFFFFFF
            fl = (t << 17) | 1 | (0x10000000 if name in persistent else 0)
            n = self.alloc(12)
            self.w32(n, k)
            self.w32(n + 4, raw)
            self.w32(n + 8, fl)
            nodes.append((k, n, name in persistent))
        nodes.sort()
        cont, _ = self.vec([n for _, n, _ in nodes])
        h = self.alloc(128)
        self.w32(h + 4, cont)
        pv = h + O.DAS_TYPEVEC + 20 * 4
        parr = self.alloc(4 * len(nodes) + 4)
        pn = [n for _, n, p in nodes if p]
        for i, n in enumerate(pn):
            self.w32(parr + 4 * i, n)
        self.w32(pv, parr)
        self.w32(pv + 4, parr + 4 * len(pn))
        return h


# (id, name) rows of the synthetic input-event table; ids differ from index+1 on purpose
INPUT_EVENTS = [(0x11, b"INPUT_EVENT_START"), (0x22, b"INPUT_EVENT_BACK"), (0x33, b"INPUT_EVENT_UP")]
AFTER_TERMINATOR = b"INPUT_EVENT_HIDDEN"     # planted after the id-0 row: must never be found

# a synthetic BaseWorldFlow graph (worldflow_fsm model shape); ids NOT in sorted order
SYN_GRAPH = {"name": "BaseWorldFlow", "start": "50", "file": "synthetic.xml", "states": [
    {"id": "50", "kind": "GameFSMState", "script": "S_32", "transitions": [{"dest": "921174500"}]},
    {"id": "921174500", "kind": "GameFSMState", "script": "S_36E801E4", "transitions": [{"dest": "105"}]},
    {"id": "105", "kind": "GameFSMState", "script": "S_69", "transitions": [{"dest": "107"}]},
    {"id": "107", "kind": "GameFSMState", "script": "S_6B",
     "transitions": [{"dest": "105"}, {"dest": "50"}, {"dest": "921174500"}]},
]}
SYN_GRAPHS = {"BaseWorldFlow": SYN_GRAPH}


def cap_setter_code(off):
    return b"".join(be32(x) for x in (0xC8030008, 0xD8230000 | off, 0xFF000800, 0x4C990020, 0xD8230008, 0x4E800020))


def cap_add_code(off):
    # 0x82204470: lbz; clrlwi; cmplwi; beqlr; lfd f0,8(r3); fadd; lfd f13,D(r3); stfd f0,8(r3)
    return b"".join(be32(x) for x in (0x89630022, 0x556B07FE, 0x2B0B0000, 0x4D9A0020, 0xC8030008,
                                      0xFC01002A, 0xC9A30000 | off, 0xD8030008))


def base_attrs(status=None, found=None, chapter=3, xp_reqs=None, extra=None):
    status = status or [0] * 9
    found = found or [0] * 9
    a = {}
    for i in range(9):
        a[b"objectiveStatus%d" % i] = (5, status[i])
        a[b"objectiveFound%d" % i] = (5, found[i])
    a[b"numObjectives"] = (0, 288)
    a[b"currentHeroPointChapter"] = (0, chapter)
    a[b"numberOfHeroPointChapters"] = (0, 20)
    reqs = xp_reqs or {1: 0, 2: 7500, 3: 19000, 4: 25000}
    for c, v in reqs.items():
        a[b"heroPointChapter%02dRequirements" % c] = (0, v)
    for nm, v in ((b"GoToNextChapter", 0), (b"triggersAvailable", 1), (b"waitingForLoad", 0),
                  (b"objectiveActivated", 0), (b"triggerEngaged", 0), (b"MaxMetropolisHealth", 25000)):
        a[nm] = (0, v)
    a[b"objectiveWorldFlowName"] = (6, b"")
    a[b"ME01Weight"] = (1, 1.0)
    if extra:
        a.update(extra)
    return a


def make_world(status=None, xp=19000.0, health=25000, quit_flag=0, triggers=None, chapter=3,
               flow_name=b"BaseWorldFlow", stat_names=None, extra=None, fsm_cur=3, fsm_prev=2,
               fsm_graph=SYN_GRAPH, fsm_vt=None, cap=25000.0, cap_off=0x28, add_off=None,
               cap_setter=None):
    w = World()
    attrs = base_attrs(status=status, chapter=chapter, extra=extra)
    persist = {k for k in attrs if k.startswith(b"objectiveStatus") or k.startswith(b"objectiveFound")
               or k == b"MaxMetropolisHealth"}
    handle = w.das(attrs, persist)
    # world-flow manager with the base flow in slot 0 and one other flow in slot 2
    mgr = w.alloc(64)
    defs = w.alloc(8 * 3)
    for i, nm in enumerate((b"Other_Transition", flow_name, b"Z_Transition")):
        w.w32(defs + 8 * i + 4, w.cstr(nm))
    slots = w.alloc(32)
    for i, nm in ((0, flow_name), (2, b"MetropolisWorldFlow")):
        f = w.alloc(128)
        w.img.plant(f + O.FLOW_NAME, nm + b"\0")
        w.w32(f + O.FLOW_NAME_HASH, _HASH31(nm))
        w.w32(f + O.FLOW_DAS, handle if i == 0 else w.das({b"dummy": (0, 1)}))
        w.w32(slots + 4 * i, f)
        if i == 0 and fsm_graph is not None:
            w.w32(f + O.FLOW_FSM, build_fsm(w, fsm_graph, fsm_cur, fsm_prev, fsm_vt))
            w.base_flow = f
    w.w32(mgr + O.WFM_DEFS, defs)
    w.w32(mgr + O.WFM_SLOTS, slots)
    w.w32(mgr + O.WFM_CUR_DEF, 1)
    w.w32(O.G_WORLDFLOW_MGR, mgr)
    # objective manager, metro
    om = w.alloc(8)
    w.img.plant(om, bytes([0, quit_flag, 0, 0, 1, 0, 1]))
    w.w32(O.G_OBJ_MGR, om)
    mo = w.alloc(32)
    w.w32(mo + 4, health)
    w.w32(mo + 8, 25000)
    w.img.plant(mo + 16, struct.pack(">f", 1.0))
    w.w32(O.G_METRO, mo)
    # stats
    names = stat_names or [(b"TotalPlayTime", 7, 12.5), (b"CurrentXP", 7, xp), (b"PreviousXP", 1, 7500.0)]
    # the CurrentXP class: shared getter, SetStatCap at vt+20, clamping AddToStat at vt+12
    w.cap_vt = w.alloc(64)
    w.img.plant(w.cap_vt + 40, be32(w.stat_getter))
    w.cap_setter = w.alloc(32)
    w.img.plant(w.cap_setter, cap_setter if cap_setter is not None else cap_setter_code(cap_off))
    w.img.plant(w.cap_vt + 20, be32(w.cap_setter))
    w.cap_add = w.alloc(32)
    w.img.plant(w.cap_add, cap_add_code(cap_off if add_off is None else add_off))
    w.img.plant(w.cap_vt + 12, be32(w.cap_add))
    objs = []
    for nm, fl, val in names:
        e = w.alloc(64)
        if nm == b"CurrentXP":
            w.w32(e, w.cap_vt)
            w.img.plant(e + cap_off, struct.pack(">d", cap))
            w.xp_stat = e
        else:
            w.w32(e, w.stat_vt)
        w.img.plant(e + 8, struct.pack(">d", val))
        w.w32(e + O.STAT_NAME, w.cstr(nm))
        w.img.plant(e + O.STAT_FLAGS, bytes([fl]))
        objs.append(e)
    arr = w.alloc(4 * len(objs))
    for i, e in enumerate(objs):
        w.w32(arr + 4 * i, e)
    st = w.alloc(8)
    w.w32(st, arr)
    w.w32(st + 4, len(objs))
    w.w32(O.G_STATS, st)
    # powerups
    pus = [(b"ComboLevelOne", 1, 1), (b"ComboLevelTwo", 2, 1), (b"ComboLevelThree", 3, 0), (b"StaminaLevelOne", 1, 0)]
    parr = w.alloc(4 * len(pus))
    for i, (nm, lvl, bought) in enumerate(pus):
        e = w.alloc(40)
        w.w32(e + O.PU_NAME, w.cstr(nm))
        w.img.plant(e + O.PU_LEVEL, bytes([lvl, bought]))
        w.w32(parr + 4 * i, e)
    pl = w.alloc(12)
    w.w32(pl + 4, parr)
    w.w32(pl + 8, len(pus))
    w.w32(O.G_POWERUPS, pl)
    # filter registry with OBJ (row 1), TRIGGERS (row 2), ENGAGED (row 3)
    nametbl = w.alloc(0xC0)
    rows = w.alloc(48 * 4)
    rowbits = {1: (4, 0x08), 2: (11, 0x80), 3: (0, 0x04)}   # (byte, bit) per row
    for r, (byte, bit) in rowbits.items():
        w.img.plant(rows + 48 * r + byte, bytes([bit]))
    w.w32(nametbl + 72, rows)
    size = 7
    keys, vals = w.alloc(4 * size), w.alloc(4 * size)
    for i in range(size):
        w.w32(keys + 4 * i, 0xFFFFFFFF)
    for nm, r in ((b"OBJ", 1), (b"TRIGGERS", 2), (b"ENGAGED", 3)):
        k = _HASH31(nm)
        slot, step, n = k % size, 1, 0
        while w.img.read(keys + 4 * slot, 4) != be32(0xFFFFFFFF):
            slot, step, n = (slot + step) % size, step * 2, n + 1
        idx = w.alloc(4)
        w.w32(idx, r)
        w.w32(keys + 4 * slot, k)
        w.w32(vals + 4 * slot, idx)
    tbl = nametbl + 84
    w.w32(tbl + 8, size)
    w.w32(tbl + 12, vals)
    w.w32(tbl + 24, keys)
    mask = bytearray(32)
    for r in (1, 2):
        byte, bit = rowbits[r]
        mask[byte] |= bit
    filt = w.alloc(80)
    w.img.plant(filt, bytes(mask))
    members = []
    w.trigger_actors = []
    for t in (triggers or []):
        a = w.alloc(0x90)
        w.w32(a, w.actor_vt)
        groups = bytearray(mask)
        if t.get("engaged"):
            groups[0] |= 0x04
        w.img.plant(a + O.ACTOR_GROUPS, bytes(groups))
        tattrs = {b"objectiveIndex": (0, t["idx"]), b"triggerIsRepeatable": (0, t.get("rep", 0)),
                  b"triggerDistance": (1, t.get("dist", 100.0)), b"objectiveAvailable": (0, t.get("avail_attr", 1)),
                  b"objectiveWorldFlowName": (6, t.get("flow", b"OBJ%d" % t["idx"]))}
        if "deps" in t:
            tattrs[b"triggerDependencies"] = (6, t["deps"])
        if "chreq" in t:
            tattrs[b"heroPointChapterRequired"] = (0, t["chreq"])
        if "event" in t:
            tattrs[b"triggerEvent"] = (6, t["event"])
        w.w32(a + O.ACTOR_DAS, w.das(tattrs))
        brain = w.alloc(0x40)
        w.w32(a + O.ACTOR_BRAIN, brain)
        w.w32(brain + O.BRAIN_ACTOR, a)
        mach = w.alloc(8)
        w.w32(brain + O.BRAIN_MACHINE, mach)
        state = w.alloc(0x30)
        w.w32(mach, state)
        w.w32(state, O.VT_TRIGGER_STATE)
        w.w32(state + O.STATE_FLAGS, t.get("fsm_flags", 0))
        w.img.plant(state + O.STATE_DISTANCE, struct.pack(">f", t.get("fsm_dist", t.get("dist", 100.0))))
        w.w32(state + O.STATE_EVENT, t.get("fsm_event", _event_id(t.get("event"))))
        members.append(a)
        w.trigger_actors.append(a)
    _, marr = w.vec(members)
    w.w32(filt + 64, marr)
    w.w32(filt + 68, marr + 4 * len(members))
    fs = w.alloc(0x40)
    nb = 5
    buckets = w.alloc(4 * (nb + 1))
    node = w.alloc(8)
    w.w32(node, filt)
    w.w32(buckets + 4 * (_FNV1(bytes(mask)) % nb), node)
    w.w32(fs + 0x14 + 4, buckets)
    w.w32(fs + 0x14 + 8, nb)
    reg = w.alloc(16)
    w.w32(reg, nametbl)
    w.w32(reg + 8, fs)
    w.w32(O.G_FILTER_REGISTRY, reg)
    w.finish_atoms()
    w.mgr, w.handle, w.metro, w.objmgr, w.filter = mgr, handle, mo, om, filt
    return w


def _event_id(name):
    """Test-side model of sub_8229A568 over INPUT_EVENTS (independent of objstate.py)."""
    for ident, nm in INPUT_EVENTS:
        if name and nm == name:
            return ident
    return 0


def build_fsm(w, graph, cur, prev, vt=None):
    """A flow FSM as FsmParseElementCb_sub_82293398 leaves it: states in document order,
    transitions carry destination INDICES."""
    ids = [int(st["id"]) for st in graph["states"]]
    arr = w.alloc(4 * len(ids))
    for i, st in enumerate(graph["states"]):
        so = w.alloc(0x20)
        w.w32(so, 0x8201731C)
        tr = st["transitions"]
        ta = w.alloc(4 * len(tr) + 4)
        for k, t in enumerate(tr):
            to = w.alloc(8)
            w.w32(to, 0x82017BF8)
            w.w32(to + O.FTRANS_DEST, ids.index(int(t["dest"])))
            w.w32(ta + 4 * k, to)
        w.w32(so + O.FSTATE_NTRANS, len(tr))
        w.w32(so + O.FSTATE_TRANS, ta)
        w.w32(arr + 4 * i, so)
    fsm = w.alloc(0x40)
    w.w32(fsm, O.VT_FLOW_FSM if vt is None else vt)
    w.w32(fsm + O.FSM_START, ids.index(int(graph["start"])))
    w.w32(fsm + O.FSM_CUR, cur)
    w.w32(fsm + O.FSM_PREV, prev)
    w.w32(fsm + O.FSM_COUNT, len(ids))
    w.w32(fsm + O.FSM_STATES, arr)
    w.fsm, w.fsm_states = fsm, arr
    return fsm


def status_words(bits):
    w = [0] * 9
    for b in bits:
        w[b >> 5] |= 1 << (b & 31)
    return w


DEFAULT_TRIGGERS = [
    {"idx": 5, "deps": b"147,2", "flow": b"WARobj01", "dist": 250.0},          # needs 147 and 2
    {"idx": 12, "deps": b"147", "chreq": 4, "flow": b"BIZobj03", "avail_attr": 0},  # needs chapter 4 XP
    {"idx": 2, "deps": b"147", "rep": 0, "engaged": True, "fsm_flags": 1, "avail_attr": 0},     # won, not repeatable
]


def snap(w, docs=None, graphs=SYN_GRAPHS):
    return O.snapshot(w.img, docs, graphs)


def verdict_of(s, idx):
    for m in s["triggers"]["members"]:
        if m.get("objectiveIndex") == idx:
            return m["verdict"]
    raise KeyError(idx)


class PrimitiveTest(unittest.TestCase):
    def test_fnv1_is_multiply_then_xor(self):
        self.assertEqual(O.fnv1(b"objectiveStatus0"), 0x17E63154)   # key seen in the golden DAS
        self.assertEqual(O.fnv1(b"objectiveStatus4"), 0x17E63150)
        self.assertEqual(O.fnv1(b"CurrentXP"), 0x5883A968)

    def test_hash31_signed_chars(self):
        self.assertEqual(O.hash31(b"ab"), (31 * 97 + 98))
        self.assertEqual(O.hash31(b"\xe9"), (0x100000000 - 23) & 0xFFFFFFFF)  # (signed char)0xE9 = -23

    def test_atoi_lenient(self):
        self.assertEqual(O.atoi_lenient(b"147"), 147)
        self.assertEqual(O.atoi_lenient(b"  x12y"), 12)
        self.assertEqual(O.atoi_lenient(b"-7"), -7)
        self.assertEqual(O.atoi_lenient(b""), 0)
        self.assertEqual(O.atoi_lenient(b"\xb5" + b"9"), 9)
        self.assertEqual(O.atoi_lenient(b"4294967297"), 1)            # low 32 bits

    def test_tokenizer_matches_sub_8224DF00(self):
        T = lambda s: list(O.tokenize_deps(s))  # noqa: E731
        self.assertEqual(T(b"22,11"), [22, 11])
        self.assertEqual(T(b"147"), [147])
        self.assertEqual(T(b""), [])
        self.assertEqual(T(b",5"), [])            # comma at the string start stops the loop
        self.assertEqual(T(b"5,"), [5])           # trailing comma: ++pos reaches strlen
        self.assertEqual(T(b"3,,4"), [3, 0, 4])   # empty token atoi("") = 0
        self.assertEqual(T(b"1, 2"), [1, 2])

    def test_fctidz_low32(self):
        self.assertEqual(O.fctidz_low32(19000.9), 19000)
        self.assertEqual(O.fctidz_low32(-1.0), 0xFFFFFFFF)
        self.assertEqual(O.fctidz_low32(float("nan")), 0)
        self.assertEqual(O.fctidz_low32(1e30), 0xFFFFFFFF)


class SnapshotTest(unittest.TestCase):
    def setUp(self):
        self.w = make_world(status=status_words([147, 2]), triggers=DEFAULT_TRIGGERS)
        self.s = snap(self.w)

    def test_all_structural_checks_pass(self):
        failed = [c for c in self.s["checks"] if not c["ok"]]
        self.assertEqual(failed, [])

    def test_fields(self):
        s = self.s
        self.assertEqual(s["flows"]["current_flow_name"], "BaseWorldFlow")
        self.assertEqual(s["flows"]["loaded"], ["BaseWorldFlow", "MetropolisWorldFlow"])
        self.assertEqual(s["progress"]["won"], [2, 147])
        self.assertEqual(s["progress"]["currentHeroPointChapter"], 3)
        self.assertEqual(s["progress"]["chapterRequirements"]["04"], 25000)
        self.assertEqual(s["progress"]["miniEventWeights"], {"ME01Weight": 1.0})
        self.assertEqual(s["xp"]["CurrentXP"], 19000.0)
        self.assertEqual(s["metro"]["health"], 25000)
        self.assertEqual(s["objective_mgr"]["miniEventsEnabled"], 1)
        self.assertEqual(s["powerup_levels"], {"Combo": 2, "Stamina": 0})
        saved = {x["name"] for x in s["save_projection"]["stats"]}
        self.assertEqual(saved, {"TotalPlayTime", "CurrentXP"})          # PreviousXP flag 1: not saved
        self.assertEqual(len(s["save_projection"]["flow_persistent"]), 19)

    def test_verdicts(self):
        s = self.s
        self.assertIs(verdict_of(s, 5)["available"], True)
        v12 = verdict_of(s, 12)
        self.assertIs(v12["available"], False)
        self.assertIn("CurrentXP 19000 < heroPointChapter04Requirements 25000", v12["reasons"])
        v2 = verdict_of(s, 2)
        self.assertIs(v2["available"], False)
        self.assertEqual(v2["reasons"], ["objective 2 already won and not repeatable"])

    def test_trigger_fsm_and_groups(self):
        m = {x["objectiveIndex"]: x for x in self.s["triggers"]["members"]}
        self.assertTrue(m[2]["in_ENGAGED"])
        self.assertFalse(m[5]["in_ENGAGED"])
        self.assertEqual(m[2]["fsm"]["disabled_bit0"], 1)
        self.assertTrue(m[5]["fsm"]["distance_matches_attr"])

    def test_objstatus4_repack(self):
        w = make_world(status=status_words([34, 147]) , triggers=[])
        s = snap(w)
        rec = [x for x in s["save_projection"]["flow_persistent"] if x["name"] == "objectiveStatus4"][0]
        # 147 = word4 bit19 stays; 141 (bit13) mirrors won 34 -> set; 146/148 mirror 12/44 -> clear
        self.assertEqual(rec["hi_word"], "0x%08X" % ((1 << 19) | (1 << 13)))

    def test_static_docs(self):
        import json
        import tempfile
        docs = [{"at": "0x1", "attrs": {"objectiveIndex": "7", "triggerDependencies": "2",
                                        "triggerIsRepeatable": "0"}, "types": {}},
                {"at": "0x2", "attrs": {"objectiveIndex": "8", "triggerDependencies": "99",
                                        "triggerIsRepeatable": "0"}, "types": {}},
                {"at": "0x3", "attrs": {"objectiveIndex": "9", "triggerDependencies": "",
                                        "heroPointChapterRequired": "3", "triggerIsRepeatable": "0"},
                 "types": {"heroPointChapterRequired": "Integer"}}]
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump(docs, f)
        try:
            s = snap(self.w, [f.name])
        finally:
            os.unlink(f.name)
        st = {a["objectiveIndex"]: a["verdict"]["available"] for a in s["availability"]
              if a["source"].startswith("static")}
        self.assertEqual(st, {7: True, 8: False, 9: True})

    def test_diff_reports_story_changes(self):
        w2 = make_world(status=status_words([147]), triggers=DEFAULT_TRIGGERS)
        d = O.diff(self.s, snap(w2))
        self.assertTrue(any(x.startswith("progress.won") for x in d))
        self.assertTrue(any("availability live idx=2" in x for x in d))
        self.assertEqual(O.diff(self.s, copy.deepcopy(self.s)), [])


class RuleTest(unittest.TestCase):
    def V(self, trig, **kw):
        w = make_world(triggers=[trig], **kw)
        return verdict_of(snap(w), trig["idx"])

    def test_dependency(self):
        self.assertIs(self.V({"idx": 5, "deps": b"147"}, status=status_words([147]))["available"], True)
        v = self.V({"idx": 5, "deps": b"147,3"}, status=status_words([147]))
        self.assertEqual(v["reasons"], ["dependency 3 not won"])
        self.assertIs(self.V({"idx": 5, "deps": b",3"})["available"], True)    # game never tests 3

    def test_xp(self):
        t = {"idx": 6, "chreq": 3}
        self.assertIs(self.V(t, xp=19000.0)["available"], True)
        self.assertIs(self.V(t, xp=18999.99)["available"], False)
        self.assertIs(self.V({"idx": 6, "chreq": 0}, xp=0.0)["available"], True)     # >0 required
        self.assertIs(self.V({"idx": 6, "chreq": -2}, xp=0.0)["available"], True)
        self.assertIs(self.V(t, xp=-5.0)["available"], True)       # (u32)-5 >= 19000: unsigned compare
        self.assertIs(self.V({"idx": 6, "chreq": 9}, xp=1e9)["available"], True)   # missing req -> 0

    def test_xp_stat_is_prefix_match(self):
        # FindElementByNameHash uses strncmp(name, stat, strlen(name)): 'CurrentXPBonus' listed first wins
        names = [(b"CurrentXPBonus", 7, 0.0), (b"CurrentXP", 7, 50000.0)]
        self.assertIs(self.V({"idx": 6, "chreq": 3}, stat_names=names)["available"], False)

    def test_missing_xp_stat_is_undefined(self):
        v = self.V({"idx": 6, "chreq": 3}, stat_names=[(b"Other", 7, 1.0)])
        self.assertIsNone(v["available"])

    def test_won_repeatable(self):
        st = status_words([6])
        self.assertIs(self.V({"idx": 6, "rep": 0}, status=st)["available"], False)
        self.assertIs(self.V({"idx": 6, "rep": 1}, status=st)["available"], True)

    def test_quit_and_metro(self):
        self.assertIs(self.V({"idx": 6}, quit_flag=1)["available"], False)
        self.assertIs(self.V({"idx": 6}, health=0)["available"], False)
        self.assertIs(self.V({"idx": 6}, health=1)["available"], True)
        self.assertIs(self.V({"idx": 6}, health=0xFFFFFFF0)["available"], True)   # lwz+fcfid: u32, never < 0

    def test_game_flag_triangulation(self):
        w = make_world(triggers=[{"idx": 6, "deps": b"3", "avail_attr": 1}])
        s = snap(w)
        bad = [c for c in s["checks"] if not c["ok"]]
        self.assertEqual(len(bad), 1)
        self.assertIn("objectiveAvailable==0", bad[0]["name"])


class ImageMutationTest(unittest.TestCase):
    """A damaged or foreign image must be flagged, never read as plausible state."""

    def failed(self, w):
        return [c["name"] for c in snap(w)["checks"] if not c["ok"]]

    def test_foreign_build_fails_fingerprint(self):
        w = make_world(triggers=DEFAULT_TRIGGERS)
        w.w32(O.VT_TRIGGER_STATE + 4, 0x82000000)
        self.assertTrue(any("sub_822737D0" in n for n in self.failed(w)))

    def test_unsorted_das_is_flagged(self):
        w = make_world(triggers=[])
        cont = struct.unpack(">I", w.img.read(w.handle + 4, 4))[0]
        b = struct.unpack(">I", w.img.read(cont, 4))[0]
        x, y = w.img.read(b, 4), w.img.read(b + 4, 4)
        w.img.plant(b, y)
        w.img.plant(b + 4, x)
        self.assertTrue(any("sorted by key" in n for n in self.failed(w)))

    def test_flow_hash_mismatch_is_flagged(self):
        w = make_world(triggers=[])
        slots = struct.unpack(">I", w.img.read(w.mgr + O.WFM_SLOTS, 4))[0]
        f = struct.unpack(">I", w.img.read(slots, 4))[0]
        w.w32(f + O.FLOW_NAME_HASH, 1234)
        self.assertTrue(any("name hash" in n for n in self.failed(w)))

    def test_wrong_fsm_state_class_is_not_read(self):
        w = make_world(triggers=[{"idx": 5, "fsm_flags": 1}])
        a = w.trigger_actors[0]
        brain = struct.unpack(">I", w.img.read(a + O.ACTOR_BRAIN, 4))[0]
        mach = struct.unpack(">I", w.img.read(brain + O.BRAIN_MACHINE, 4))[0]
        st = struct.unpack(">I", w.img.read(mach, 4))[0]
        w.w32(st, 0x82017C5C)
        m = snap(w)["triggers"]["members"][0]
        self.assertFalse(m["fsm"]["ok"])
        self.assertNotIn("flags", m["fsm"])

    def test_fsm_distance_mismatch_is_flagged(self):
        w = make_world(triggers=[{"idx": 5, "dist": 100.0, "fsm_dist": 0.4}])
        self.assertTrue(any("triggerDistance" in n for n in self.failed(w)))

    def test_unreadable_pointer_is_reported(self):
        w = make_world(triggers=[])
        w.w32(O.G_STATS, 0x7FFF0000)
        s = snap(w)
        self.assertTrue(any(c["name"] == "stats" and not c["ok"] for c in s["checks"]))

    def test_metro_max_disagreement_is_flagged(self):
        w = make_world(triggers=[])
        w.w32(w.metro + 8, 30000)
        self.assertTrue(any("MaxMetropolisHealth" in n for n in self.failed(w)))

    def test_unsupported_getter_is_refused(self):
        w = make_world(triggers=[])
        w.w32(w.stat_getter, 0xC8230010)          # lfd f1,16(r3): still decodable, reads +16
        s = snap(w)
        self.assertNotEqual(s["xp"]["CurrentXP"], 19000.0)
        w.w32(w.stat_getter, 0x38600000)          # not a getter at all
        s = snap(w)
        self.assertIsNone(s["xp"]["CurrentXP"])


class VerifierRuleTest(unittest.TestCase):
    """Edge cases planted by the independent verifier (iter-463), each re-derived from the
    retail disassembly of sub_822737D0 / sub_8224DF00 / sub_82224EE0, not from objstate.py."""

    def v(self, **kw):
        trig = kw.pop("trig")
        w = make_world(triggers=[trig], **kw)
        return w, verdict_of(snap(w), trig["idx"])

    def test_metro_health_is_unsigned(self):
        # lwz / std / lfd / fcfid: 0x80000000 is +2147483648.0, never <= 0
        _, v = self.v(status=status_words([147, 2]), health=0x80000000, trig={"idx": 5, "deps": b"147,2"})
        self.assertIs(v["available"], True)

    def test_negative_dependency_reads_a_missing_word(self):
        # atoi("-1") = -1 -> r4 0xFFFFFFFF -> srwi 5 -> 'objectiveStatus134217727' absent -> 0
        _, v = self.v(status=status_words([147, 2]), trig={"idx": 5, "deps": b"-1"})
        self.assertIs(v["available"], False)

    def test_leading_comma_tests_nothing(self):
        # strchr returns the string start -> sub_8224DF00 returns 0 before any atoi
        _, v = self.v(status=status_words([147]), trig={"idx": 5, "deps": b",2"})
        self.assertIs(v["available"], True)

    def test_xp_low32_wraps(self):
        # fctidz then stfiwx keeps the low word: 2^32+5 -> 5 < 25000; -1.0 -> 0xFFFFFFFF (cmplw)
        _, v = self.v(status=status_words([147]), xp=float(2 ** 32 + 5), trig={"idx": 12, "deps": b"147", "chreq": 4})
        self.assertIs(v["available"], False)
        _, v = self.v(status=status_words([147]), xp=-1.0, trig={"idx": 12, "deps": b"147", "chreq": 4})
        self.assertIs(v["available"], True)

    def test_requirement_and_bits_come_from_the_current_flow(self):
        # sub_82218CC8 is called with FindEntryByNameIn8Slot(mgr)+12 and sub_82224EE0 reads the
        # current flow: switching the current def to a loaded flow without the attributes makes
        # every bit read 0 (so the dependency fails)
        w = make_world(status=status_words([147, 2]), triggers=[{"idx": 5, "deps": b"147,2"}])
        defs = struct.unpack(">I", w.img.read(w.mgr + O.WFM_DEFS, 4))[0]
        w.w32(defs + 8 * 2 + 4, w.cstr(b"MetropolisWorldFlow"))
        w.w32(w.mgr + O.WFM_CUR_DEF, 2)
        self.assertIs(verdict_of(snap(w), 5)["available"], False)

    def test_requirement_is_looked_up_in_the_current_flow(self):
        # current def -> a loaded flow without heroPointChapter04Requirements: sub_82218CC8
        # returns 0, so CurrentXP 0 passes (the base flow would say 25000)
        w = make_world(status=status_words([147]), xp=0.0, triggers=[{"idx": 12, "chreq": 4}])
        defs = struct.unpack(">I", w.img.read(w.mgr + O.WFM_DEFS, 4))[0]
        w.w32(defs + 8 * 2 + 4, w.cstr(b"MetropolisWorldFlow"))
        w.w32(w.mgr + O.WFM_CUR_DEF, 2)
        v = verdict_of(snap(w), 12)
        self.assertIs(v["available"], True, v)

    def test_repeatable_is_tested_for_zero_only(self):
        # cntlzw(rep) >> 5: only rep == 0 blocks a won objective
        _, v = self.v(status=status_words([147, 5]), trig={"idx": 5, "deps": b"147", "rep": 2})
        self.assertIs(v["available"], True)

    def test_negative_chapter_requirement_skips_the_flow_lookup(self):
        # cmpwi r5,0 / ble: a negative heroPointChapterRequired never reaches the current-flow
        # name read, so a missing current flow does not make the verdict undefined
        w = make_world(status=status_words([147]), triggers=[{"idx": 12, "chreq": -3}])
        w.w32(w.mgr + O.WFM_CUR_DEF, 2)          # Z_Transition: not loaded
        self.assertIs(verdict_of(snap(w), 12)["available"], True)

    def test_repack_pairs_are_ordered(self):
        # PackObjectiveStatusBits_sub_82165638: 141<-34, 146<-12, 148<-44 (li 0x8D,0x22,0x92,0xC,0x94,0x2C)
        w = make_world(status=status_words([34, 44, 147]), triggers=[])
        fp = {x["name"]: x for x in snap(w)["save_projection"]["flow_persistent"]}
        hw = int(fp["objectiveStatus4"]["hi_word"], 16)
        self.assertEqual(((hw >> 13) & 1, (hw >> 18) & 1, (hw >> 20) & 1, (hw >> 19) & 1), (1, 0, 1, 1))

    def test_metro_save_record_is_floored_at_the_threshold(self):
        # SaveRecords_WriteMetroHealth_sub_82186F70: max_u32(health, fctidz(f32(max) * lfs(threshold bits)))
        w = make_world(status=status_words([147]), health=1000, triggers=[],
                       extra={b"MetroHealthSaveThreshold": (1, 0.2)})
        m = snap(w)["save_projection"]["metro"]
        self.assertEqual((m["floor"], m["value"]), (5000, 5000))
        w = make_world(status=status_words([147]), health=0x80000000, triggers=[],
                       extra={b"MetroHealthSaveThreshold": (1, 0.2)})
        self.assertEqual(snap(w)["save_projection"]["metro"]["value"], 0x80000000)   # cmplw: unsigned
        w = make_world(status=status_words([147]), health=1000, triggers=[],
                       extra={b"MetroHealthSaveThreshold": (6, None)})              # string node -> 0
        self.assertEqual(snap(w)["save_projection"]["metro"]["value"], 1000)

    def test_non_int_max_health_node_is_a_failed_check_not_a_crash(self):
        w = make_world(status=status_words([147]), triggers=[])
        cont = struct.unpack(">I", w.img.read(w.handle + 4, 4))[0]
        b, e = struct.unpack(">II", w.img.read(cont, 8))
        for p in range(b, e, 4):
            n = struct.unpack(">I", w.img.read(p, 4))[0]
            if struct.unpack(">I", w.img.read(n, 4))[0] == _FNV1(b"MaxMetropolisHealth"):
                w.w32(n + 8, (6 << 17) | 1 | 0x10000000)
        s = snap(w)
        bad = [c["name"] for c in s["checks"] if not c["ok"]]
        self.assertIn("metro max (runtime) == DAS MaxMetropolisHealth", bad)
        fp = {x["name"]: x for x in s["save_projection"]["flow_persistent"]}
        self.assertEqual(fp["MaxMetropolisHealth"]["hi_word"], "0x00000000")   # writer stores types 0/1/5 only


def failed_names(s):
    return [c["name"] for c in s["checks"] if not c["ok"]]


class FlowFsmTest(unittest.TestCase):
    """fsm+8 (current index) -> state id through the document order of the graph (phase 1b)."""

    def test_index_maps_to_state_id_in_document_order(self):
        s = snap(make_world(triggers=[]))
        b = s["base_fsm"]
        self.assertEqual((b["index"], b["state_id"], b["prev_index"], b["prev_state_id"]), (3, 107, 2, 105))
        self.assertTrue(b["graph_verified"])
        base = [x for x in s["flow_fsms"] if x["flow"] == "BaseWorldFlow"][0]
        self.assertEqual(base["start_state_id"], 50)
        self.assertEqual(base["transitions_checked"], 4)
        self.assertEqual(failed_names(s), [])

    def test_unsorted_ids_are_not_sorted(self):
        # index 1 is 921174500 (document order), not 105 (sorted order)
        s = snap(make_world(triggers=[], fsm_cur=1, fsm_prev=0))
        self.assertEqual(s["base_fsm"]["state_id"], 921174500)

    def test_not_running(self):
        s = snap(make_world(triggers=[], fsm_cur=-1, fsm_prev=-1))
        self.assertEqual((s["base_fsm"]["index"], s["base_fsm"]["state_id"]), (-1, None))
        self.assertEqual(failed_names(s), [])

    def test_index_out_of_range_is_flagged(self):
        s = snap(make_world(triggers=[], fsm_cur=9))
        self.assertTrue(any("current index" in n for n in failed_names(s)))

    def test_live_transition_mismatch_is_flagged_and_unmapped(self):
        w = make_world(triggers=[])
        st = struct.unpack(">I", w.img.read(w.fsm_states + 4 * 3, 4))[0]
        ta = struct.unpack(">I", w.img.read(st + O.FSTATE_TRANS, 4))[0]
        t0 = struct.unpack(">I", w.img.read(ta, 4))[0]
        w.w32(t0 + O.FTRANS_DEST, 3)                  # 107 -> 107 instead of 107 -> 105
        s = snap(w)
        self.assertFalse(s["base_fsm"]["graph_verified"])
        self.assertIsNone(s["base_fsm"]["state_id"])
        self.assertTrue(any("transition list" in n for n in failed_names(s)))

    def test_state_count_mismatch_is_flagged(self):
        g = copy.deepcopy(SYN_GRAPH)
        g["states"].append({"id": "114", "kind": "GameFSMState", "script": None, "transitions": []})
        s = snap(make_world(triggers=[]), graphs={"BaseWorldFlow": g})
        self.assertTrue(any("state count" in n for n in failed_names(s)))
        self.assertIsNone(s["base_fsm"]["state_id"])

    def test_wrong_fsm_class_is_flagged(self):
        s = snap(make_world(triggers=[], fsm_vt=0x82008A00))
        self.assertTrue(any("flow-FSM vtable" in n for n in failed_names(s)))

    def test_no_graph_reports_the_index_only(self):
        s = snap(make_world(triggers=[]), graphs=None)
        self.assertEqual(s["base_fsm"]["index"], 3)
        self.assertIsNone(s["base_fsm"]["state_id"])
        self.assertEqual(failed_names(s), [])

    def test_diff_reports_the_fsm_state(self):
        a, b = snap(make_world(triggers=[])), snap(make_world(triggers=[], fsm_cur=2, fsm_prev=1))
        self.assertIn("base_fsm.state_id: 107 -> 105", O.diff(a, b))


class StatCapTest(unittest.TestCase):
    """CurrentXP cap: decoded from the class's SetStatCap slot (vt+20, 0x821F1288 shape)."""

    def test_cap_decoded_and_triangulated(self):
        s = snap(make_world(triggers=[]))
        self.assertEqual(s["xp"]["CurrentXP_cap"], 25000.0)
        self.assertEqual(s["xp"]["cap_expected"], 25000)       # heroPointChapter04Requirements (chapter 3)
        self.assertEqual(failed_names(s), [])
        other = [x for x in s["stats"] if x["name"] == "TotalPlayTime"][0]
        self.assertEqual((other["cap"], other["cap_offset"]), (None, None))   # `blr`: no cap

    def test_cap_disagreeing_with_the_requirement_is_flagged(self):
        s = snap(make_world(triggers=[], cap=19000.0))
        self.assertTrue(any("CurrentXP cap == heroPointChapter04Requirements" in n for n in failed_names(s)))

    def test_zero_cap_means_state_105_never_ran(self):
        s = snap(make_world(triggers=[], cap=0.0))
        self.assertEqual(s["xp"]["CurrentXP_cap"], 0.0)
        self.assertNotIn("cap_expected", s["xp"])
        self.assertEqual(failed_names(s), [])

    def test_offset_is_read_from_the_code(self):
        s = snap(make_world(triggers=[], cap_off=0x30))
        self.assertEqual(s["xp"]["CurrentXP_cap"], 25000.0)
        self.assertEqual([x for x in s["stats"] if x["name"] == "CurrentXP"][0]["cap_offset"], 0x30)

    def test_add_slot_at_another_offset_is_flagged(self):
        s = snap(make_world(triggers=[], add_off=0x30))
        self.assertTrue(any("AddToStat" in n for n in failed_names(s)))

    def test_unsupported_setter_is_refused(self):
        s = snap(make_world(triggers=[], cap_setter=be32(0xC8030008) + be32(0xD8230028) + be32(0x4E800020) * 4))
        self.assertTrue(any(n.startswith("stat cap") for n in failed_names(s)))
        self.assertIsNone(s["xp"]["CurrentXP_cap"])


class TriggerEventTest(unittest.TestCase):
    """state+0x18 == sub_8229A568(triggerEvent), 0 for an empty/missing string."""

    def fsm(self, trig):
        s = snap(make_world(status=status_words([147]), triggers=[trig]))
        return s, s["triggers"]["members"][0]["fsm"]

    def test_named_event_links(self):
        s, f = self.fsm({"idx": 5, "event": b"INPUT_EVENT_UP"})
        self.assertEqual((f["event_id"], f["event_expected"], f["triggerEvent"]), (0x33, 0x33, "INPUT_EVENT_UP"))
        self.assertEqual(failed_names(s), [])

    def test_empty_event_is_zero(self):
        s, f = self.fsm({"idx": 5, "event": b""})
        self.assertEqual((f["event_id"], f["event_expected"]), (0, 0))
        self.assertEqual(failed_names(s), [])
        s, f = self.fsm({"idx": 5})
        self.assertEqual((f["event_id"], f["triggerEvent"]), (0, None))

    def test_mismatch_is_flagged(self):
        s, f = self.fsm({"idx": 5, "event": b"INPUT_EVENT_UP", "fsm_event": 0})
        self.assertFalse(f["event_matches_attr"])
        self.assertTrue(any("+0x18" in n for n in failed_names(s)))
        s, f = self.fsm({"idx": 5, "fsm_event": 0x11})        # no triggerEvent but a nonzero id
        self.assertTrue(any("+0x18" in n for n in failed_names(s)))

    def test_unknown_and_post_terminator_names_give_zero(self):
        s, f = self.fsm({"idx": 5, "event": b"INPUT_EVENT_NOPE"})
        self.assertEqual(f["event_expected"], 0)
        s, f = self.fsm({"idx": 5, "event": AFTER_TERMINATOR, "fsm_event": 0})
        self.assertEqual(f["event_expected"], 0)                # the id-0 row ends the walk
        self.assertEqual(failed_names(s), [])


def _golden(name):
    import golden
    return os.path.isfile(golden.DUMPS.get(name, ""))


@unittest.skipUnless(_golden("sound_gameplay") and os.path.isfile(O.DEFAULT_AST), "golden dump / retail AST absent")
class GoldenFieldTest(unittest.TestCase):
    """The phase-1b fields on the Xenia goldens (rank 2 evidence)."""

    @classmethod
    def setUpClass(cls):
        cls.graphs = O.load_fsm_graphs(O.DEFAULT_AST)
        cls.gp = O.snapshot(O.open_image("xenia:sound_gameplay"), None, cls.graphs)

    def test_gameplay_is_in_InObjective(self):
        b = self.gp["base_fsm"]
        self.assertEqual((b["index"], b["state_id"], b["count"], b["graph_verified"]), (3, 107, 44, True))
        self.assertEqual(b["prev_state_id"], 2023575322)      # ObjectiveTeleport -> InObjective
        self.assertEqual(self.gp["checks_failed"], 0)

    def test_gameplay_cap_is_chapter_2_requirement(self):
        self.assertEqual(self.gp["xp"]["CurrentXP_cap"], 7500.0)
        self.assertEqual(self.gp["xp"]["cap_expected"], 7500)

    def test_gameplay_triggers_have_no_input_event(self):
        ev = [(m["fsm"] or {}).get("event_id") for m in self.gp["triggers"]["members"]]
        self.assertEqual(ev, [0, 0])

    @unittest.skipUnless(_golden("sound_menu") and _golden("nosound"), "menu goldens absent")
    def test_menu_goldens_wait_at_press_start(self):
        for g in ("sound_menu", "nosound"):
            s = O.snapshot(O.open_image("xenia:" + g), None, self.graphs)
            self.assertEqual((s["base_fsm"]["index"], s["base_fsm"]["state_id"]), (17, 1793378647), g)
            self.assertEqual(s["xp"]["CurrentXP_cap"], 0.0, g)
            self.assertEqual(s["checks_failed"], 0, g)



# ---------------------------------------------------------------- failure states (open items 15-17)
def _fs(i, *dests):
    return {"id": str(i), "kind": "GameFSMState", "script": None, "transitions": [{"dest": str(d)} for d in dests]}


# the retail loss subgraph's shape (worldflow_fsm.py dump BaseWorldFlow --lua), ids NOT sorted
SYN_FAIL_GRAPH = {"name": "BaseWorldFlow", "start": "50", "file": "synthetic_fail.xml", "states": [
    _fs(50, 105), _fs(105, 1807139120), _fs(1807139120, 107, 159151984),
    _fs(107, 105, 796478688, 159151984, 2048434547),
    _fs(796478688, 114, 1233294090, 2057594878), _fs(2057594878, 2112419636, 1233294090), _fs(2112419636, 114),
    _fs(114, 2050165252, 1531063701, 1398601461), _fs(2048434547, 796478688),
    _fs(159151984, 1360459395, 168103572), _fs(1360459395, 168103572), _fs(168103572, 1531063701, 1398601461),
    _fs(1233294090, 105), _fs(1398601461, 105), _fs(1531063701, 105), _fs(2050165252, 107),
]}
LOSS_OBJECTIVE = [114, 796478688, 2048434547, 2057594878, 2112419636]
LOSS_GAME_OVER = [159151984, 168103572, 1360459395]


def _fail_index(sid):
    return [int(x["id"]) for x in SYN_FAIL_GRAPH["states"]].index(sid)


class FailureStateTest(unittest.TestCase):
    """The objective-failure marker (live run 1: MSTobj02 failed while idle; its end also wrote
    objectiveActivated = 0, the same as a win) and the checks that do not apply in a loss state
    or outside Metropolis (open item 17)."""

    def world(self, state, mini=0, act=0, health=25000, triggers=None):
        return make_world(triggers=triggers or [], fsm_graph=SYN_FAIL_GRAPH, fsm_cur=_fail_index(state),
                          fsm_prev=_fail_index(107), health=health,
                          extra={b"miniEventEngaged": (0, mini), b"objectiveActivated": (0, act)})

    def snap(self, w):
        return snap(w, graphs={"BaseWorldFlow": SYN_FAIL_GRAPH})

    def test_closure_on_the_synthetic_graph(self):
        self.assertEqual(O.base_fail_states(SYN_FAIL_GRAPH), (LOSS_OBJECTIVE, LOSS_GAME_OVER))

    def test_missing_entry_or_stop_state_gives_no_sets(self):
        for drop in (796478688, 1233294090, 159151984):
            g = copy.deepcopy(SYN_FAIL_GRAPH)
            g["states"] = [x for x in g["states"] if int(x["id"]) != drop]
            self.assertEqual(O.base_fail_states(g), (None, None), drop)
        self.assertEqual(O.base_fail_states(SYN_GRAPH), (None, None))
        self.assertEqual(O.base_fail_states(None), (None, None))

    @unittest.skipUnless(os.path.isfile(O.DEFAULT_AST), "retail AST absent")
    def test_retail_graph(self):
        g = O.load_fsm_graphs(O.DEFAULT_AST)["BaseWorldFlow"]
        self.assertEqual(O.base_fail_states(g), (LOSS_OBJECTIVE, LOSS_GAME_OVER))

    def test_verdict_table(self):
        sets = (LOSS_OBJECTIVE, LOSS_GAME_OVER)
        V = lambda sid, act, mini: O.failure_verdict(sid, act, mini, sets)  # noqa: E731
        self.assertEqual((V(2112419636, 0, 0)["kind"], V(2112419636, 0, 0)["objective_failed"]), ("objective_loss", True))
        self.assertEqual((V(796478688, 0, 1)["kind"], V(796478688, 0, 1)["objective_failed"]), ("metro_event_loss", False))
        self.assertEqual((V(114, 0, None)["kind"], V(114, 0, None)["objective_failed"]), ("loss_mini_unread", False))
        self.assertEqual((V(168103572, 0, 1)["kind"], V(168103572, 0, 1)["objective_failed"]), ("game_over", True))
        self.assertFalse(V(2112419636, 1, 0)["objective_failed"])            # objectiveActivated must be 0
        for sid in (107, 105, 1807139120, 926189043, 1233294090, None):    # wins pass 926189043 -> ... 1233294090
            self.assertEqual((V(sid, 0, 0)["kind"], V(sid, 0, 0)["objective_failed"]), (None, False), sid)
        self.assertFalse(O.failure_verdict(2112419636, 0, 0, (None, None))["objective_failed"])

    def test_snapshot_in_a_loss_state(self):
        s = self.snap(self.world(2112419636))
        self.assertEqual(s["base_fsm"]["state_id"], 2112419636)
        self.assertEqual((s["failure"]["kind"], s["failure"]["objective_failed"]), ("objective_loss", True))
        self.assertEqual(s["failure"]["graph_sets"], {"objective_loss": LOSS_OBJECTIVE, "game_over": LOSS_GAME_OVER})
        s = self.snap(self.world(107, act=1))
        self.assertEqual((s["failure"]["kind"], s["failure"]["objective_failed"]), (None, False))
        s = self.snap(self.world(796478688, mini=1))
        self.assertEqual((s["failure"]["kind"], s["failure"]["objective_failed"]), ("metro_event_loss", False))

    def test_availability_triangulation_does_not_apply_in_a_loss_state(self):
        # live run 1, S01: metro health 0 after the meteor storm, the game's objectiveAvailable still
        # 1 from the trigger's last enter (105); no enter runs until Retry
        trig = [{"idx": 6, "avail_attr": 1}]
        s = self.snap(self.world(2112419636, health=0, triggers=trig))
        self.assertEqual(failed_names(s), [])
        v = verdict_of(s, 6)
        self.assertIs(v["available"], False)
        self.assertIn("n/a: base FSM in loss state 2112419636", v["triangulation"])
        # the same disagreement outside a loss state is still a failed check
        s = self.snap(self.world(107, act=1, health=0, triggers=trig))
        self.assertTrue(any("objectiveAvailable==0" in n for n in failed_names(s)))

    def test_metro_max_check_only_in_metropolis(self):
        w = make_world(triggers=[], health=8000)
        w.w32(w.metro + 8, 10000)                       # the Warworld arena's object (live run 1, W1)
        self.assertTrue(any("MaxMetropolisHealth" in n for n in failed_names(snap(w))))
        slots = struct.unpack(">I", w.img.read(w.mgr + O.WFM_SLOTS, 4))[0]
        w.w32(slots + 8, 0)                             # MetropolisWorldFlow unloaded
        s = snap(w)
        self.assertEqual(failed_names(s), [])
        self.assertEqual(s["metro"]["max_check"], "n/a: MetropolisWorldFlow is not loaded")
        self.assertFalse(s["metro"]["in_metropolis"])


LIVE1 = os.environ.get("SR_LIVE1_DIR") or os.path.join(REPO, "archive", "v1_devtools", "skip", "live1")
LIVE1_SNAPS = {"P0": "devtool_20261001-092142_xenia/P0.snap", "S01": "devtool_20261001-092142_xenia/S01_pass_0.snap",
               "C0": "devtool_20261001-093452_xenia/C0_MSTobj02_running.snap",
               "C1": "devtool_20261001-093452_xenia/C1_MSTobj03_running.snap",
               "W1": "devtool_20261001-093819_xenia/W1_WARobj01_engaged.snap"}


@unittest.skipUnless(os.path.isfile(os.path.join(LIVE1, LIVE1_SNAPS["S01"])) and os.path.isfile(O.DEFAULT_AST),
                     "live run 1 snapshots (archive/v1_devtools/skip/live1 or SR_LIVE1_DIR) / retail AST absent")
class LiveRun1Test(unittest.TestCase):
    """Rank-1 images: the .snap files of live run 1 on retail in Xenia (DEVTOOLS_DESIGN 6.7)."""

    @classmethod
    def setUpClass(cls):
        g = O.load_fsm_graphs(O.DEFAULT_AST)
        cls.s = {k: O.snapshot(O.open_image("snap:" + os.path.join(LIVE1, v)), None, g) for k, v in LIVE1_SNAPS.items()}

    def test_the_failure_is_marked_and_nothing_else_is(self):
        s01 = self.s["S01"]
        self.assertEqual((s01["base_fsm"]["state_id"], s01["progress"]["objectiveActivated"]), (2112419636, 0))
        self.assertEqual((s01["failure"]["kind"], s01["failure"]["objective_failed"]), ("objective_loss", True))
        for k in ("P0", "C0", "C1", "W1"):
            self.assertEqual((self.s[k]["base_fsm"]["state_id"], self.s[k]["failure"]["objective_failed"]), (107, False), k)

    def test_no_failed_checks_in_any_live_snapshot(self):
        # before open item 17: S01 failed the availability triangulation, W1 the metro max check
        for k, s in self.s.items():
            self.assertEqual(failed_names(s), [], k)
        self.assertEqual(self.s["W1"]["metro"]["max_check"], "n/a: MetropolisWorldFlow is not loaded")


class MutantTest(unittest.TestCase):
    """Replace one ported primitive with a plausible wrong version; the suite must catch it."""

    MUTANTS = {
        "fnv1a": ("fnv1", lambda d: _fnv1a(d)),
        "hash31_unsigned": ("hash31", lambda d: _hash31u(d)),
        "tokenizer_ignores_leading_comma": ("tokenize_deps", lambda s: iter(
            [O.atoi_lenient(t) for t in s.split(b",") if t])),
        "atoi_no_sign": ("atoi_lenient", lambda s: abs(_orig_atoi(s))),
        "fctidz_python_int": ("fctidz_low32", lambda x: 0 if x != x else (int(x) if abs(x) < 2 ** 32 else 0) & 0xFFFFFFFF
                              if x >= 0 else 0),
        "strncmp_exact": ("c_strncmp_eq", lambda a, b, n: a == b),
        "fsm_ids_sorted": ("fsm_state_ids", lambda g: sorted(_orig_ids(g))),
        "cap_fixed_offset": ("decode_stat_cap", lambda m, e: _cap_fixed(m, e)),
        "cap_setter_ignored": ("decode_stat_cap", lambda m, e: (None, None, 0)),
        "event_row_index": ("input_event_id", lambda m, name: _event_row_index(m, name)),
        "event_no_terminator": ("input_event_id", lambda m, name: _event_no_term(m, name)),
        # open items 15-17 (the failure marker and the scoped checks)
        "fail_closure_unstopped": ("FAIL_STOP", ()),
        "fail_entry_dropped": ("FAIL_ENTRY_OBJECTIVE", (796478688,)),
        "failure_ignores_mini": ("failure_verdict", lambda sid, act, mini, sets: _orig_fv(sid, act, 0, sets)),
        "failure_ignores_activated": ("failure_verdict", lambda sid, act, mini, sets: _orig_fv(sid, 0, mini, sets)),
        "failure_any_state": ("failure_verdict", lambda sid, act, mini, sets: dict(
            _orig_fv(sid, act, mini, sets), objective_failed=act == 0)),
    }

    def run_suite(self):
        suite = unittest.TestSuite()
        for cls in (PrimitiveTest, SnapshotTest, RuleTest, ImageMutationTest, VerifierRuleTest,
                    FlowFsmTest, StatCapTest, TriggerEventTest, FailureStateTest):
            suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(cls))
        with open(os.devnull, "w") as devnull:
            res = unittest.TextTestRunner(stream=devnull, verbosity=0).run(suite)
        return len(res.failures) + len(res.errors)

    def test_every_mutant_is_killed(self):
        self.assertEqual(self.run_suite(), 0, "the unmutated suite must pass")
        survivors = []
        for name, (attr, fn) in self.MUTANTS.items():
            orig = getattr(O, attr)
            setattr(O, attr, fn)
            try:
                if self.run_suite() == 0:
                    survivors.append(name)
            finally:
                setattr(O, attr, orig)
        self.assertEqual(survivors, [])

    def test_scope_mutants_are_killed(self):
        """The two scopes added for live run 1 (open item 17), removed one at a time."""
        from unittest import mock
        self.assertEqual(self.run_suite(), 0)
        orig_metro = O.Snap.metro
        with mock.patch.object(O.Snap, "metro", lambda self, das_by, in_metropolis=True: orig_metro(self, das_by, True)):
            self.assertNotEqual(self.run_suite(), 0, "metro max check unscoped survived")
        orig_fv = O.failure_verdict
        with mock.patch.object(O, "failure_verdict", lambda *a: dict(orig_fv(*a), kind=None)):
            self.assertNotEqual(self.run_suite(), 0, "availability triangulation unscoped survived")

    def test_availability_clause_mutants_are_killed(self):
        """Mutate the verdict function's clauses via its inputs contract."""
        orig = O.availability
        survivors = []

        def drop(reason_prefix):
            def mutant(r, attrs):
                v = orig(r, attrs)
                v["reasons"] = [x for x in v["reasons"] if not x.startswith(reason_prefix)]
                if v["available"] is not None:
                    v["available"] = not v["reasons"]
                return v
            return mutant
        for pfx in ("dependency", "CurrentXP", "objective", "quit", "metro"):
            O.availability = drop(pfx)
            try:
                if self.run_suite() == 0:
                    survivors.append(pfx)
            finally:
                O.availability = orig
        self.assertEqual(survivors, [])


_orig_atoi = O.atoi_lenient
_orig_fv = O.failure_verdict
_orig_ids = O.fsm_state_ids
_orig_cap = O.decode_stat_cap


def _cap_fixed(m, e):
    cap, off, fn = _orig_cap(m, e)
    return (m.f64(e + 0x28), 0x28, fn) if off is not None else (cap, off, fn)


def _event_row_index(m, name):
    h = O.fnv1(name)
    for k in range(191):
        if m.u32(O.G_INPUT_EVENTS + 12 * k) == 0:
            return 0
        if m.u32(O.G_INPUT_EVENTS + 12 * k + 8) == h:
            return k + 1
    return 0


def _event_no_term(m, name):
    h = O.fnv1(name)
    for k in range(191):
        row = O.G_INPUT_EVENTS + 12 * k
        if len(m.img.read(row, 12)) < 12:
            return 0
        if m.u32(row + 8) == h:
            return m.u32(row)
    return 0


def _fnv1a(d):
    h = 0x811C9DC5
    for c in d:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def _hash31u(d):
    h = 0
    for c in d:
        h = (31 * h + c) & 0xFFFFFFFF
    return h


if __name__ == "__main__":
    unittest.main(verbosity=1)
