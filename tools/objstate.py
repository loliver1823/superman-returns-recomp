#!/usr/bin/env python3
"""objstate.py - offline reader of the objective / world-progress state in a memory image.

WHY: level skip (and any claim about story state) needs one reader that sees the Xenia
golden and our own build through IDENTICAL code (memory/feedback_tool_verification_protocol.md;
critique section B4 in the devtools design). This tool is that reader. It never writes guest
memory and never calls guest code; every lookup below is a Python port of the retail
function that performs it, cited by address, so the tool reads what the game itself would
read. Unknown or structurally inconsistent memory is reported as such, never guessed.

Images (same spec as tools/havok_snapshot.py, whose image layer this imports):
  xenia:<name|path>  Xenia minidump (golden names: nosound | sound_menu | sound_gameplay)
  ours:<path>        full minidump of SupermanReturns.exe (guest base auto-detected)
  snap:<file>        the touched guest pages only (tools/guest_snap.py); written by --write-snap.
                     A read outside the captured pages is a failed check, never a zero.

Usage:
  python tools/objstate.py snap --image xenia:sound_gameplay [--label L] [--out DIR]
                               [--trigger-docs a.json,b.json | --no-trigger-docs]
                               [--ast baseworl.AST | --no-ast] [--ledger L.json]
                               [--write-snap FILE.snap]
  python tools/objstate.py diff A.json B.json      # story fields + availability verdicts
  python tools/objstate.py summary A.json          # re-print the summary of a snapshot

Output: <out>/<label>.json (schema objstate/1) + <label>.txt summary.
Default out dir: archive/v1_devtools/objstate/ (gitignored evidence ground).

What is read (critique B4 compared set):
  - world-flow manager (loaded flows, current flow) and the current flow's DAS: every node,
    named through the game's own atom table; objectiveStatus0..8 / objectiveFound0..8,
    currentHeroPointChapter, chapter requirements, GoToNextChapter, triggersAvailable,
    waitingForLoad, objectiveActivated, triggerEngaged, the engage scratch attributes,
    MaxMetropolisHealth, ME%02dWeight, min/maxTimeBetweenTriggerSpawns, ...
  - objective manager bytes (*0x8296DF5C): replay / quit-to-FE / engage / mini-events
  - Metropolis health object (*0x8296DF50): health, max, percent
  - stats (*0x8296DF70): every stat, its flags, value; flag&4 = saved (CurrentXP ...)
  - powerups (*0x8296DF6C): byte +35 (purchased; the save's "entity byte +35")
  - the save-record projection: what AutoSaveGame (sub_8218E3A8) would capture now
  - OBJ|TRIGGERS members: attributes, objectiveAvailable, filter-group membership
    (ENGAGED), trigger FSM state flags, TriggerGate power
  - the availability verdict of every objective: a pure port of sub_822737D0 over live
    triggers and (optionally) the static trigger docs parsed from baseworl.AST/metropol.AST
  - every loaded flow's FSM (flow+0x6C): current / previous state index (fsm+8 / +0xC); for
    BaseWorldFlow the index is mapped to the graph's state id (107 = InObjective, 105 =
    ReactivateTriggers ...) through tools/worldflow_fsm.py, after proving the mapping on the
    image (every live transition list == the decoded graph)
  - the CurrentXP cap (+0x28, decoded from the class's SetStatCap slot) and each trigger
    FSM's input-event id (state+0x18, linked to triggerEvent through sub_8229A568)
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_OUT = os.path.join(REPO, "archive", "v1_devtools", "objstate")
DEFAULT_TRIGGER_DOCS = [os.path.join(REPO, "archive", "v1_devtools", n)
                        for n in ("baseworl_das_docs.json", "metropol_das_docs.json")]
DEFAULT_AST = os.path.join(REPO, "Superman Returns (USA)", "DATA", "baseworl.AST")  # BaseWorldFlow graph
SCHEMA = "objstate/1"

# ================================================================== image layer ====
# ADAPTER POINT: the image layer (Xenia dumps, our minidumps with guest-base detection,
# synthetic images for tests) lives in tools/havok_snapshot.py. If that module is absent
# or its API changed, the fallback below still serves Xenia dumps (via tools/golden.py)
# and synthetic images; 'ours:' then needs the havok_snapshot image layer.
sys.path.insert(0, HERE)
try:
    from havok_snapshot import Image, SyntheticImage, open_image as _hs_open_image  # noqa: E402
    IMAGE_LAYER = "havok_snapshot"
except Exception:  # pragma: no cover - exercised only when havok_snapshot is missing
    IMAGE_LAYER = "fallback"

    class Image:
        kind = "abstract"

        def read(self, va, n):
            raise NotImplementedError

        def describe(self):
            return {"kind": self.kind}

    class SyntheticImage(Image):
        kind = "synthetic"

        def __init__(self):
            self.pages = {}

        def plant(self, va, data):
            for i, byte in enumerate(data):
                a = va + i
                self.pages.setdefault(a >> 12, bytearray(4096))[a & 0xFFF] = byte

        def read(self, va, n):
            out = bytearray()
            for a in range(va, va + n):
                pg = self.pages.get(a >> 12)
                if pg is None:
                    break
                out.append(pg[a & 0xFFF])
            return bytes(out)

    class _GoldenImage(Image):
        kind = "xenia"

        def __init__(self, spec):
            import golden
            self.g = golden.GoldenDump(spec)

        def read(self, va, n):
            return self.g.read(va, n)

        def describe(self):
            return {"kind": self.kind, "path": os.path.abspath(self.g.path)}

    def _hs_open_image(spec, base=None):
        if spec.startswith("xenia:"):
            return _GoldenImage(spec[6:])
        if spec.startswith("snap:"):
            import guest_snap
            return guest_snap.SnapImage(spec[5:])
        raise ValueError("ours: images need tools/havok_snapshot.py (image layer adapter point)")


def open_image(spec, base=None):
    return _hs_open_image(spec, base=base)


class ReadError(Exception):
    """A read the game would perform hit unmapped / unreadable memory."""


class Mem:
    """Strict big-endian reads over an Image. Every miss raises ReadError."""

    def __init__(self, img):
        self.img = img
        self.reads = 0
        self.pages = set()   # read ledger: every 4 KiB guest page a port touched (DEVTOOLS_DESIGN 6.2)

    def _touch(self, va, n):
        for pg in range((va & 0xFFFFFFFF) >> 12, (((va & 0xFFFFFFFF) + max(n, 1) - 1) >> 12) + 1):
            self.pages.add(pg)

    def ledger(self):
        """The touched pages merged into [start, end) guest ranges."""
        out = []
        for pg in sorted(self.pages):
            if out and out[-1][1] == pg << 12:
                out[-1][1] += 0x1000
            else:
                out.append([pg << 12, (pg << 12) + 0x1000])
        return out

    def raw(self, va, n):
        self.reads += 1
        self._touch(va, n)
        b = self.img.read(va & 0xFFFFFFFF, n)
        if len(b) != n:
            raise ReadError("unreadable 0x%08X+%d" % (va & 0xFFFFFFFF, n))
        return b

    def u8(self, va):
        return self.raw(va, 1)[0]

    def u16(self, va):
        return struct.unpack(">H", self.raw(va, 2))[0]

    def u32(self, va):
        return struct.unpack(">I", self.raw(va, 4))[0]

    def f32(self, va):
        return struct.unpack(">f", self.raw(va, 4))[0]

    def f64(self, va):
        return struct.unpack(">d", self.raw(va, 8))[0]

    def cstr(self, va, maxlen=256):
        """C string at va (b'' for an empty string). None for a null pointer."""
        if not va:
            return None
        out = bytearray()
        while len(out) < maxlen:
            chunk = self.img.read((va + len(out)) & 0xFFFFFFFF, 32)
            self._touch(va + len(out), 32)
            if not chunk:
                raise ReadError("unterminated/unreadable string at 0x%08X" % va)
            i = chunk.find(b"\0")
            if i >= 0:
                out += chunk[:i]
                return bytes(out)
            out += chunk
        raise ReadError("string at 0x%08X longer than %d" % (va, maxlen))


# ============================================================= guest layout ====
# Globals (each cited by a retail function that reads it)
G_WORLDFLOW_MGR = 0x8296DF74   # sub_821F6588 returns it; FindEntryByNameIn8Slot_sub_821F68E8(mgr)
G_OBJ_MGR = 0x8296DF5C         # objective mgr (7 bytes, DogmaAllocInitSingleton_sub_821EB978);
                               # sub_822737D0 reads byte +1; sub_8222C060 arg
G_METRO = 0x8296DF50           # metro health object: ScriptCmd_GetMetropolisHealth_sub_821E86B8 (+4),
                               # ScriptCmd_SetMaxMetropolisHealth_sub_821F9730 (+8, +16)
G_STATS = 0x8296DF70           # stat list {elems*, count}: FindElementByNameHash_sub_821F16B8, sub_82165198
G_POWERUPS = 0x8296DF6C        # powerup list {?, elems*, count}: sub_82165250, sub_821F0DE0
G_FILTER_REGISTRY = 0x8296DD08  # trigger-filter registry: FindTriggerFilterByNameList_sub_821CC2E0
G_ATOMS = 0x829A23B8           # string-atom map: AptStringAtomFindValue_sub_826CE950
G_FSM_CURRENT = 0x8296E0E8     # "current FSM state" (written by ~200 state actions; informational only)
G_INPUT_EVENTS = 0x82912B68    # {u32 id, char* name, u32 FNV-1(name)} x <=191, id 0 ends: sub_8229A568
OFF_NAME_OBJ_TRIGGERS = 0x82912090   # off_82912090[0] -> "OBJ|TRIGGERS" (Objective_FindTriggerActrByIndex_sub_822102F0)
OFF_NAME_OBJECTIVE_INDEX = 0x829120BC  # off_829120BC[0] -> "objectiveIndex" (sub_822102F0)
F_ZERO_820013B8 = 0x820013B8   # flt_820013B8, the metro-health threshold compared in sub_822737D0

# WorldFlowMgr (FindEntryByNameIn8Slot_sub_821F68E8, WorldFlowMgr_FindFlowByName_sub_821F6850)
WFM_DEFS = 0x08                # a1[2]: defs array, 8-byte entries, name* at +4
WFM_SLOTS = 0x20               # a1[8]: 8 loaded-flow slots
WFM_CUR_DEF = 0x2C             # a1[11]: current def index
FLOW_NAME = 0x0C               # inline char[] (strcmp(slot+12, ...))
FLOW_NAME_HASH = 0x4C          # hash31(name): sub_821F6850 (*(flow+76))
FLOW_DAS = 0x68                # DAS doc handle: WorldFlow_GetCurrentFlowAttr_sub_822101B0 (+104)
FLOW_FSM = 0x6C                # FSM runner: WorldFlow_Update_sub_82225E60 ticks *(flow+108) via vt[1]
# Flow FSM (FlowFsm_Tick_sub_82293108, FSM tick sub_8278C500, FsmSetState_sub_8278C1C0,
# FsmWalkTransitions_sub_8278C448, parse FsmParseElementCb_sub_82293398)
VT_FLOW_FSM = 0x8200E5E4       # [data] class of every flow FSM in the goldens; vt[1] == sub_82293108 (fingerprinted)
FSM_START = 0x04               # start-state index (sub_8278C500 enters state[+4] while +0x14 is set)
FSM_CUR = 0x08                 # current state INDEX, -1 = not running (sub_82293108 gate; sub_8278C1C0 stores it)
FSM_PREV = 0x0C                # previous index (sub_8278C1C0: +0xC := old +8)
FSM_COUNT = 0x18               # state count (bounds check in sub_82293108 / sub_8278C500)
FSM_STATES = 0x1C              # state* array; index = order of <state> elements in the graph XML
                               # (the parse pushes each id and uses count-1 as its index)
FSTATE_NTRANS = 0x08           # transition count (sub_8278C448)
FSTATE_TRANS = 0x0C            # transition* array (sub_8278C448)
FTRANS_DEST = 0x04             # destination state INDEX handed to FsmSetState (sub_8278C448)
DAS_CONTAINER = 0x04           # handle+4 -> {begin,end} vector of node*: sub_822101B0 / sub_821E6DA8
DAS_TYPEVEC = 0x08             # handle+8+20*i: node vectors by type bit (DasDocHandle_GetNodesByTypeBit_sub_821E6D68)
DAS_PERSIST_BIT = 16           # sub_82181E28 asks for type bit 16 (the saved nodes)
NODE_KEY, NODE_VALUE, NODE_FLAGS = 0, 4, 8  # AptStoreDisplayObjectValue_sub_82177310 (*a2, a2[1], a2[2]>>17&0x7F)
DAS_TYPE_NAMES = {0: "int", 1: "float", 3: "vector", 5: "bitfield", 6: "string"}
# types 0/1/5 per sub_82177310; 6 = atom-hash string (sub_821E6F30); 3 = pointer [data, teleportPosition]

# Actors / triggers
ACTOR_DAS = 0x80               # actor+128: DAS handle (sub_822737D0 via ActorStack_GetCurrentActor_sub_82262AD0,
                               # sub_82210228 via vt+36('ACTR'))
ACTOR_GROUPS = 0x30            # 256-bit filter-group membership (EventBucketRemoveEntryByMask_sub_821CCDA8 +48)
ACTOR_ID = 0x58                # actor+88 id (sub_8225D488 copies member+88 into the FSM state +32)
ACTOR_POWERS = 0x68            # actor+104 power manager (sub_8222C060 v24[26]; sub_82273B40 +104)
ACTOR_BRAIN = 0x84             # [data] component whose +60 == actor (FSM holder); guarded
BRAIN_ACTOR = 0x3C             # [data] brain+60 == actor (same +0x3C as ActorStack_GetCurrentActor)
BRAIN_MACHINE = 0x1C           # [data] brain+28 -> state machine; *(machine) = current state; guarded
VT_TRIGGER_STATE = 0x82017790  # trigger FSM state vtable: slot+4 = sub_822737D0 (enter), +8 = sub_82273B40 (update)
STATE_FLAGS = 0x04             # sub_822737D0 clears the low 3 bits then |=1 when unavailable; sub_82273B40 |=1 on engage
STATE_DISTANCE = 0x14          # sub_822737D0 stfs triggerDistance at 0x14 (structural cross-check)
STATE_EVENT = 0x18             # sub_822737D0 (0x82273880..0x822738B4): triggerEvent non-empty ->
                               # sub_8229A568(name) (input-event id), else 0 (r23); sub_82273B40 needs
                               # it == 0 or the dispatched input event
STATE_ENTITY_ID = 0x20         # sub_82273B40: ScreenRegistryLookup(*(a1+32))
FOURCC_ACTR = 0x41435452

# Stats (FindElementByNameHash_sub_821F16B8, sub_82165198)
STAT_NAME = 0x1C               # char* at +28
STAT_FLAGS = 0x22              # byte +34; &4 = written to the save (sub_82165198)
STAT_VALUE_VTSLOT = 0x28       # vt+40 getter (sub_82165198, sub_822737D0); decoded, see stat_value()
STAT_SETCAP_VTSLOT = 0x14      # vt+20: ScriptCmd_SetStatCap_sub_821F2688 calls it; decoded, see decode_stat_cap()
STAT_ADD_VTSLOT = 0x0C         # vt+12: AddToStat (ScriptCmd_AddToStatByName_sub_821F2130); clamps at the cap
STAT_SET_VTSLOT = 0x20         # vt+32: ScriptCmd_SetStatValueByName_sub_821F1E88 calls it (0x8223FD58 =
                               # `stfd f1,8(r3); blr` for every retail class: a plain store, no clamp)
# Powerups (sub_82165250, ScriptCmd_IsPowerupPurchased_sub_82203F50)
PU_ELEMS, PU_COUNT = 0x04, 0x08  # sub_821F0DE0: *(a1+4), *(a1+8)
PU_NAME = 0x08                 # sub_821F08E8: hash31(*(e+8))
PU_PURCHASED = 0x23            # byte +35
PU_LEVEL = 0x22                # [data] byte +34 = level within the family (1..12); not read by the save
# Metro health object
METRO_HEALTH, METRO_MAX, METRO_PERCENT = 0x04, 0x08, 0x10
# Objective manager bytes (docs/ENGINE_MAP + Lua natives, see study:infra)
OBJMGR_FIELDS = {0: "replayObjective", 1: "quitToFrontEnd", 2: "engageObjective", 4: "miniEventsEnabled"}

# PackObjectiveStatusBits_sub_82165638: (dest bit in objectiveStatus4, source objective)
PACK_OBJ4_PAIRS = ((141, 34), (146, 12), (148, 44))

# Fingerprint: retail code bytes the ports mirror (a different build must fail loudly)
FINGERPRINT = [
    ("vt 0x82017790+4 == sub_822737D0", VT_TRIGGER_STATE + 4, 0x822737D0),
    ("vt 0x82017790+8 == sub_82273B40", VT_TRIGGER_STATE + 8, 0x82273B40),
    ("sub_822737D0 starts mflr r12", 0x822737D0, 0x7D8802A6),
    ("sub_82224EE0 starts mflr r12", 0x82224EE0, 0x7D8802A6),
    ("vt 0x8200E5E4+4 == FlowFsm_Tick_sub_82293108", VT_FLOW_FSM + 4, 0x82293108),
]

HEADLINE_BASE_FIELDS = [
    "currentHeroPointChapter", "numberOfHeroPointChapters", "GoToNextChapter",
    "triggersAvailable", "waitingForLoad", "objectiveActivated", "triggerEngaged",
    "objectiveComplete", "objectiveWon", "objectiveBitIndex", "objectiveWorldFlowName",
    "triggerArenaChange", "triggerReturnToMetropolis", "MaxMetropolisHealth", "MaxMetroHealthBoost",
    "minTimeBetweenTriggerSpawns", "maxTimeBetweenTriggerSpawns", "miniEventWeightReductionMultiplier",
]


# ======================================================== ported primitives ====
def fnv1(data):
    """FNV-1 (multiply, then xor) over unsigned bytes: AptResolvePathString_sub_826CD770
    (i = 16777619*i ^ c from 0x811C9DC5) and EastlHashFind_FNV32_sub_821DFA78."""
    h = 0x811C9DC5
    for c in data:
        h = ((h * 16777619) & 0xFFFFFFFF) ^ c
    return h


def _sc(c):
    return c - 256 if c >= 128 else c


def hash31(data):
    """Str_Hash31_sub_8263C508 / sub_8263C550: h = 31*h + (signed char)c, 32-bit wrap."""
    h = 0
    for c in data:
        h = (31 * h + _sc(c)) & 0xFFFFFFFF
    return h


def c_strncmp_eq(a, b, n):
    """Crt_Strncmp_sub_825F5AE8(a, b, n) == 0 for NUL-free Python bytes a, b: a NUL inside the
    first n chars of either string ends the compare (b[:n] is then shorter and unequal)."""
    return a[:n] == b[:n]


def atoi_lenient(s):
    """Str_AtoiLenient_sub_825F5380: skip to '-' or a digit (bytes >= 0x80 are negative chars,
    never digits), optional '-', accumulate digits in 64 bits, return the low 32 bits signed."""
    i, n = 0, len(s)
    while i < n and s[i] != 0x2D and not (0x30 <= s[i] <= 0x39):
        i += 1
    neg = i < n and s[i] == 0x2D
    if neg:
        i += 1
    v = 0
    while i < n and 0x30 <= s[i] <= 0x39:
        v = (v * 10 + (s[i] - 0x30)) & 0xFFFFFFFFFFFFFFFF
        i += 1
    if neg:
        v = (-v) & 0xFFFFFFFFFFFFFFFF
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def fctidz_low32(x):
    """fctidz (truncate toward zero to int64; NaN and -overflow -> 0x8000000000000000,
    +overflow -> 0x7FFFFFFFFFFFFFFF) then stfiwx/lwz = the low 32 bits (sub_822737D0)."""
    if x != x or x <= -2.0 ** 63:
        v = 0x8000000000000000
    elif x >= 2.0 ** 63:
        v = 0x7FFFFFFFFFFFFFFF
    else:
        v = int(x) & 0xFFFFFFFFFFFFFFFF
    return v & 0xFFFFFFFF


def tokenize_deps(s):
    """The triggerDependencies loop of sub_822737D0 with sub_8224DF00(str, ',', &pos, &val).
    Yields every value the game would test, in order (the game stops at the first unset bit;
    callers decide). Mirrors: pos>=strlen -> stop; strchr(&s[pos], ','): none -> atoi rest,
    pos=strlen; found at s+0 (the WHOLE string's start) -> stop; else atoi up to it, pos=comma;
    then the caller does ++pos and stops when pos >= strlen."""
    n = len(s)
    pos = 0
    if n == 0:
        return
    while True:
        if pos >= n:
            return
        j = s.find(b",", pos)
        if j < 0:
            yield atoi_lenient(s[pos:])
            pos = n
        else:
            if j == 0:
                return
            yield atoi_lenient(s[pos:j])
            pos = j
        pos += 1
        if pos >= n:
            return


def input_event_id(m, name):
    """sub_8229A568(_, name): h = sub_825F43E0(name, 0x811C9DC5) (FNV-1, unsigned bytes); walk the
    12-byte rows {id, name*, hash} at 0x82912B68 while id != 0 and at most 191 rows (the loop
    stops when 12*(k+1) >= 0x8F4); return the id of the first row whose hash matches, else 0.
    The first argument (*0x8296E8C4) is unused by the retail code."""
    h = fnv1(name)
    for k in range(0x8F4 // 12):                  # 191 rows (0x8F4 = 12 * 191)
        row = G_INPUT_EVENTS + 12 * k
        ident = m.u32(row)
        if ident == 0:
            return 0
        if m.u32(row + 8) == h:
            return ident
    return 0


# vt+20 SetStatCap shapes: the CurrentXP class (0x821F1288) and the no-op of every other class
_CAP_SETTER = (0xC8030008, 0xD8230000, 0xFF000800, 0x4C990020, 0xD8230008, 0x4E800020)
#               lfd f0,8(r3) stfd f1,D(r3) fcmpu cr6,f0,f1 blelr cr6 stfd f1,8(r3) blr


def decode_stat_cap(m, e):
    """The stat's cap, decoded from its class's SetStatCap slot (vt+20, the call target of
    ScriptCmd_SetStatCap_sub_821F2688). Retail has two shapes:
      `blr`                                   -> no cap (SetStatCap is a no-op): returns None
      `lfd f0,8(r3); stfd f1,D(r3); fcmpu cr6,f0,f1; blelr cr6; stfd f1,8(r3); blr`
                                              -> cap = f64 at +D; value := cap when value > cap
    Returns (cap or None, offset or None, setter VA). Anything else raises Unknown."""
    fn = m.u32(m.u32(e) + STAT_SETCAP_VTSLOT)
    if m.u32(fn) == 0x4E800020:
        return None, None, fn
    w = struct.unpack(">6I", m.raw(fn, 24))
    if (w[0] == _CAP_SETTER[0] and (w[1] & 0xFFFF0000) == _CAP_SETTER[1] and w[2:] == _CAP_SETTER[2:]):
        off = w[1] & 0xFFFF
        return m.f64(e + off), off, fn
    raise Unknown("stat SetStatCap slot 0x%08X has an unsupported shape (%s)" % (fn, " ".join("%08X" % x for x in w)))


def fsm_state_ids(graph):
    """Index -> state id for a decoded graph (tools/worldflow_fsm.py fsm_model). The parse
    (FsmParseElementCb_sub_82293398, 'state') pushes each <state> id and gives the state the
    index count-1, so the index is the document order of the <state> elements."""
    return [int(s["id"]) if s.get("id") is not None else None for s in graph["states"]]


# BaseWorldFlow failure states (live run 1, open item 15; decoded from baseworl.AST, the Lua is
# in `worldflow_fsm.py dump BaseWorldFlow --lua`). The objective's end runs 107's exit action,
# which writes objectiveActivated := 0 on a WIN and on a LOSS alike (DT12258), so
# objectiveActivated == 0 cannot tell them apart. The state the FSM is in can:
#   loss entries   796478688  "[base] lose objective" (107 t_7: objectiveComplete && !objectiveWon)
#                  2048434547 "force failing due to engaging trigger during objective" (-> 796478688)
#   game over      159151984  "play loss text" Fail_Game_0N (metropolis dead: from 107 or from the
#                             triggers superstate 1807139120)
# Each set is the closure of its entries over the decoded transitions, stopped at the states a
# loss shares with the rest of the game: 1233294090 (objective unload; also the win path),
# 1398601461 (loading), 1531063701 (quit), 2050165252 (objective restart = Retry).
# Retail: objective loss {796478688, 2048434547, 2057594878, 2112419636, 114 (ShowObjectiveLoss,
# the Quit/Retry popup)}; game over {159151984, 1360459395, 168103572 (ShowSandboxLoss)}.
# Metro events use the same loss states with miniEventEngaged != 0 (their loss goes on to
# 1233294090); that is not an objective failure.
FAIL_ENTRY_OBJECTIVE = (796478688, 2048434547)
FAIL_ENTRY_GAME_OVER = (159151984,)
FAIL_STOP = (1233294090, 1398601461, 1531063701, 2050165252)
STATE_TRIGGERS = 1807139120          # "[base] superman triggers": the triggers superstate after 105
STATE_IN_OBJECTIVE = 107


def base_fail_states(graph):
    """(objective-loss ids, game-over ids) of a decoded BaseWorldFlow graph (see above), or
    (None, None) when an entry or stop state is missing from the graph."""
    if not graph:
        return None, None
    ids = fsm_state_ids(graph)
    trans = {int(s["id"]): [int(t["dest"]) for t in s["transitions"] if t.get("dest") is not None]
             for s in graph["states"] if s.get("id") is not None}
    if any(x not in trans for x in FAIL_ENTRY_OBJECTIVE + FAIL_ENTRY_GAME_OVER + FAIL_STOP):
        return None, None

    def closure(entries):
        out, stack = set(), list(entries)
        while stack:
            x = stack.pop()
            if x in out or x in FAIL_STOP:
                continue
            out.add(x)
            stack += trans.get(x, [])
        return out
    obj, over = closure(FAIL_ENTRY_OBJECTIVE), closure(FAIL_ENTRY_GAME_OVER)
    if obj & over or not set(ids) >= obj | over:
        return None, None
    return sorted(obj), sorted(over)


def failure_verdict(state_id, activated, mini, graph_sets):
    """The objective-failure marker: objectiveActivated == 0 AND the base FSM in a loss state
    (objective loss with miniEventEngaged == 0, or game over). -> {kind, objective_failed}; kind is
    'objective_loss' | 'game_over' | 'metro_event_loss' | 'loss_mini_unread' (no miniEventEngaged
    node: not called a failure) | None."""
    obj, over = graph_sets
    kind = None
    if obj is not None and state_id is not None:
        if state_id in over:
            kind = "game_over"
        elif state_id in obj:
            kind = ("objective_loss" if mini == 0 else "loss_mini_unread" if mini is None
                    else "metro_event_loss")
    return {"state_id": state_id, "kind": kind,
            "objective_failed": kind in ("objective_loss", "game_over") and activated == 0}


def load_fsm_graphs(ast_path):
    """{graph name: model} from a BGFA container via tools/worldflow_fsm.py; {} when absent."""
    if not ast_path or not os.path.isfile(ast_path):
        return {}
    import worldflow_fsm
    _, _, graphs = worldflow_fsm.load(ast_path)
    return {g["name"]: g for g in graphs}


# ============================================================== the reader ====
class Unknown(Exception):
    """The game's own code would do something undefined here (null deref, garbage name)."""


class GameReader:
    """Pure ports over one image. Methods raise ReadError / Unknown; snapshot() catches."""

    def __init__(self, img):
        self.m = Mem(img)
        self.img = img
        self._atom_cache = {}

    # ---- sorted node vectors (SortedPtrVec_FindByKey_sub_821947D8) ----
    def vec_find(self, vec, key):
        """Binary search over {begin,end} of T* sorted by *(T+0). Returns the iterator VA or end."""
        m = self.m
        v3, end = m.u32(vec), m.u32(vec + 4)
        v4 = end
        if end == v3:
            return end
        for _ in range(64):
            mid = v3 + 4 * (((v4 - v3) & 0xFFFFFFFF) >> 3)
            k = m.u32(m.u32(mid))
            if k == key:
                return mid
            if k <= key:
                v3 = mid + 4
            else:
                v4 = mid
            if v4 == v3:
                return end
        raise ReadError("vec_find did not converge at 0x%08X (not a sorted vector?)" % vec)

    def resolve_path(self, container, path):
        """AptResolvePathString_sub_826CD770(container, path) -> node VA or 0. '/' descends:
        an intermediate node's +4 is the child container."""
        m = self.m
        a1 = container
        v3 = 0
        v5 = 0
        if path:
            while a1:
                if path[v5] == 0x2F:
                    it = self.vec_find(a1, fnv1(path[v3:v5]))
                    a1 = 0 if it == m.u32(a1 + 4) else m.u32(m.u32(it) + 4)
                    v3 = v5 + 1
                v5 += 1
                if v5 >= len(path):
                    break
            else:
                return 0
        if a1:
            it = self.vec_find(a1, fnv1(path[v3:]))
            if it != m.u32(a1 + 4):
                return m.u32(it)
        return 0

    def das_node(self, handle, name):
        """handle+4 container, then resolve (sub_821E6DA8 / sub_821E6F30 / sub_82218CC8 prologue)."""
        if not handle:
            return 0
        c = self.m.u32(handle + DAS_CONTAINER)
        if not c:
            return 0
        return self.resolve_path(c, name)

    def das_int(self, handle, name):
        """sub_821E6DA8: node ? *(node+4) : 0 (raw u32)."""
        n = self.das_node(handle, name)
        return self.m.u32(n + NODE_VALUE) if n else 0

    def das_str(self, handle, name):
        """sub_821E6F30: node ? AptStringAtomFindValue(*(node+4)) : 0 -> bytes or None."""
        n = self.das_node(handle, name)
        if not n:
            return None
        return self.atom(self.m.u32(n + NODE_VALUE))

    # ---- string atoms (AptStringAtomFindValue_sub_826CE950 + MapLowerBound_sub_821DD388) ----
    def atom(self, key):
        """Returns the atom's C string (bytes) or None when the key is absent (the game returns 0)."""
        if key in self._atom_cache:
            return self._atom_cache[key]
        m = self.m
        holder = m.u32(G_ATOMS)
        if not holder:
            raise ReadError("atom map 0x829A23B8 is null")
        tree = m.u32(holder)             # v14 = *a1
        header = tree + 4                # end()
        x = m.u32(tree + 12)             # root
        res = header
        for _ in range(96):
            if not x:
                break
            if m.u32(x + 16) < key:
                x = m.u32(x)
            else:
                res = x
                x = m.u32(x + 4)
        else:
            raise ReadError("atom tree walk did not terminate")
        if res == header or key < m.u32(res + 16):
            out = None
        else:
            out = m.cstr(m.u32(res + 28))
        self._atom_cache[key] = out
        return out

    # ---- world-flow manager ----
    def flow_mgr(self):
        return self.m.u32(G_WORLDFLOW_MGR)

    def current_def_name(self, mgr):
        m = self.m
        return m.cstr(m.u32(8 * m.u32(mgr + WFM_CUR_DEF) + m.u32(mgr + WFM_DEFS) + 4))

    def current_flow(self):
        """FindEntryByNameIn8Slot_sub_821F68E8(*0x8296DF74): the slot whose inline name equals
        the current def's name; 0 when none."""
        m = self.m
        mgr = self.flow_mgr()
        if not mgr:
            return 0
        target = self.current_def_name(mgr)
        slots = m.u32(mgr + WFM_SLOTS)
        for i in range(8):
            f = m.u32(slots + 4 * i)
            if f and m.cstr(f + FLOW_NAME) == target:
                return f
        return 0

    def find_flow_by_name(self, name):
        """WorldFlowMgr_FindFlowByName_sub_821F6850: match hash31(name) against flow+0x4C."""
        m = self.m
        mgr = self.flow_mgr()
        h = hash31(name) if name else 0
        slots = m.u32(mgr + WFM_SLOTS)
        for i in range(8):
            f = m.u32(slots + 4 * i)
            if f and m.u32(f + FLOW_NAME_HASH) == h:
                return f
        return 0

    def current_flow_attr(self, name):
        """WorldFlow_GetCurrentFlowAttr_sub_822101B0 -> node or 0."""
        f = self.current_flow()
        if not f:
            return 0
        return self.das_node(self.m.u32(f + FLOW_DAS), name)

    def flow_int_by_name(self, attr, flow_name):
        """sub_82218CC8(mgr, attr, flowName) via sub_822085C8 -> FindFlowByName (name non-null)."""
        f = self.find_flow_by_name(flow_name)
        if not f:
            return 0
        return self.das_int(self.m.u32(f + FLOW_DAS), attr)

    def objective_bit(self, bit):
        """WorldFlow_IsObjectiveStatusBitSet_sub_82224EE0(_, bit): 'objectiveStatus%i' % (bit>>5)
        on the current flow; 0 when the variable is missing."""
        bit &= 0xFFFFFFFF
        n = self.current_flow_attr(b"objectiveStatus%d" % (bit >> 5))
        if not n:
            return 0
        return (self.m.u32(n + NODE_VALUE) >> (bit & 31)) & 1

    # ---- stats ----
    def stat_list(self):
        m = self.m
        st = m.u32(G_STATS)
        if not st:
            return []
        arr, n = m.u32(st), m.u32(st + 4)
        if n > 4096:
            raise ReadError("stat count %d implausible" % n)
        return [m.u32(arr + 4 * i) for i in range(n)]

    def find_stat(self, name):
        """FindElementByNameHash_sub_821F16B8(*0x8296DF70, name): first stat whose name matches
        strncmp(name, statName, strlen(name)) - a PREFIX match, as in the game."""
        m = self.m
        if not m.u32(G_STATS) or not name:
            return 0
        for e in self.stat_list():
            if c_strncmp_eq(name, m.cstr(m.u32(e + STAT_NAME)) or b"", len(name)):
                return e
        return 0

    def stat_value(self, e):
        """The stat's vt+40 getter. All retail stat classes share 0x8223FD70 = `lfd f1,8(r3); blr`;
        the getter's code is decoded from the image and anything else is refused."""
        m = self.m
        fn = m.u32(m.u32(e) + STAT_VALUE_VTSLOT)
        code = m.raw(fn, 8)
        ins, ret = struct.unpack(">II", code)
        if ret == 0x4E800020 and (ins & 0xFFFF0000) == 0xC8230000:   # lfd f1, d(r3); blr
            return m.f64(e + (ins & 0xFFFF))
        raise Unknown("stat getter at 0x%08X is not `lfd f1,d(r3); blr` (%08X %08X)" % (fn, ins, ret))

    # ---- entity components ----
    def get_component(self, obj, fourcc):
        """obj->vt[+36](obj, fourcc) for the trigger class: the getter is decoded from the image.
        Supported shape (0x821E4B78): repeated {lis r11,hi; ori r11,r11,lo; cmpw cr6,r4,r11;
        beqlr cr6} then {li r3,0; blr} -> returns obj for a listed fourcc, else 0."""
        m = self.m
        fn = m.u32(m.u32(obj) + 36)
        pc = fn
        for _ in range(16):
            w = struct.unpack(">4I", m.raw(pc, 16))
            if w[0] == 0x38600000 and w[1] == 0x4E800020:
                return 0
            if ((w[0] & 0xFFFF0000) == 0x3D600000 and (w[1] & 0xFFFF0000) == 0x616B0000
                    and w[2] == 0x7F045800 and w[3] == 0x4D9A0020):
                if ((w[0] & 0xFFFF) << 16 | (w[1] & 0xFFFF)) == fourcc:
                    return obj
                pc += 16
                continue
            raise Unknown("vt+36 getter 0x%08X has an unsupported shape" % fn)
        raise Unknown("vt+36 getter 0x%08X too long" % fn)

    # ---- trigger filters ----
    def build_mask(self, nametbl, names):
        """AptBuildMaskFromNameList_sub_821B3C18: OR of the 32-byte rows of each '|' token.
        Row = *(nametbl+72) + 48*idx when HashTableLookup_sub_821DC218(nametbl+84, tok) hits,
        else the default row nametbl+40."""
        m = self.m
        mask = bytearray(32)
        for tok in names.split(b"|"):
            if not tok:
                continue
            p = self.hash_lookup(nametbl + 84, tok)
            row = (m.u32(p) * 48 + m.u32(nametbl + 72)) if p else nametbl + 40
            mask = bytearray(a | b for a, b in zip(mask, m.raw(row, 32)))
        return bytes(mask)

    def hash_lookup(self, tbl, tok):
        """HashTableLookup_sub_821DC218: key = sub_8263C550(tok, len); open addressing over
        size a1[2], keys a1[6], values a1[3]; probe step doubles; at most 10 probes."""
        m = self.m
        k = hash31(tok)
        size = m.u32(tbl + 8)
        if size == 0:
            raise ReadError("filter name table size 0 (the game traps: twllei)")
        keys, vals = m.u32(tbl + 24), m.u32(tbl + 12)
        v8, v6, v7 = k % size, 1, 0
        while m.u32(keys + 4 * v8) != k:
            v9 = v6 + v8
            v7 += 1
            v6 *= 2
            v8 = v9 % size
            if v7 >= 10:
                return 0
        return m.u32(vals + 4 * v8)

    def find_filter(self, filterset, mask):
        """FindTriggerFilterPair_sub_821CC160 with a4=0: bucket = FNV-1(mask) % n over the
        hash at filterset+0x14 (+4 buckets, +8 count), chain {value*, next}, 32-byte equality
        (EastlListFindMatchingEntry_sub_821DF328)."""
        m = self.m
        h = filterset + 0x14
        buckets, n = m.u32(h + 4), m.u32(h + 8)
        if n == 0:
            raise ReadError("filter hash bucket count 0 (the game traps)")
        node = m.u32(buckets + 4 * (fnv1(mask) % n))
        for _ in range(4096):
            if not node:
                return 0
            v = m.u32(node)
            if m.raw(v, 32) == mask:
                return v
            node = m.u32(node + 4)
        raise ReadError("filter chain did not terminate")

    def filter_members(self, filt):
        m = self.m
        b, e = m.u32(filt + 64), m.u32(filt + 68)
        n = ((e - b) & 0xFFFFFFFF) >> 2
        if n > 65536:
            raise ReadError("filter member count %d implausible" % n)
        return [m.u32(b + 4 * i) for i in range(n)]

    def power_find(self, pmgr, name):
        """PowerSys_FindPowerByName_sub_82226C50: hash31(name) in the vector_map at pmgr+4
        (8-byte {key, power*} entries; EastlVectorMap_EqualRange_sub_82243B40)."""
        m = self.m
        b, e = m.u32(pmgr + 4), m.u32(pmgr + 8)
        k = hash31(name)
        lo, hi = 0, ((e - b) & 0xFFFFFFFF) >> 3
        if hi > 4096:
            raise ReadError("power map size implausible")
        while lo < hi:                    # lower_bound (EastlLowerBoundPairU32_sub_821A7BD0)
            mid = (lo + hi) // 2
            if m.u32(b + 8 * mid) < k:
                lo = mid + 1
            else:
                hi = mid
        if lo == ((e - b) & 0xFFFFFFFF) >> 3 or k < m.u32(b + 8 * lo):
            return 0
        return m.u32(b + 8 * lo + 4)


# ================================================= availability (sub_822737D0) ====
class LiveAttrs:
    """Trigger attributes read from the live DAS handle at actor+128."""

    def __init__(self, r, handle):
        self.r, self.handle = r, handle

    def container(self):
        return self.r.m.u32(self.handle + DAS_CONTAINER) if self.handle else 0

    def exists(self, name):
        c = self.container()
        return bool(c and self.r.resolve_path(c, name))

    def int(self, name):
        return self.r.das_int(self.handle, name)

    def str(self, name):
        return self.r.das_str(self.handle, name)


class StaticAttrs:
    """Trigger attributes from a parsed asset doc (archive/v1_devtools/*_das_docs.json).
    Integers are stored as the DAS stores them (u32); an empty string behaves like a
    missing atom (both make the game skip the check: strlen 0 / null)."""

    def __init__(self, attrs, types):
        self.a, self.t = attrs, types

    def container(self):
        return 1

    def exists(self, name):
        return name.decode() in self.a

    def int(self, name):
        v = self.a.get(name.decode())
        if v is None:
            return 0
        if self.t.get(name.decode()) == "Float":
            return struct.unpack(">I", struct.pack(">f", float(v)))[0]
        return int(float(v)) & 0xFFFFFFFF

    def str(self, name):
        v = self.a.get(name.decode())
        return None if v in (None, "") else v.encode("latin1")


def availability(r, attrs):
    """Pure port of the verdict part of sub_822737D0 (TriggerFSM enter action). Returns
    {available, reasons[], inputs{}}; 'available' is None when the game's own code would be
    undefined (null CurrentXP stat, no current flow). Offsets: see the constants above.
    Reads, in the game's order:
      triggerDependencies -> tokenize_deps -> objective_bit(dep) for each (sub_82224EE0)
      heroPointChapterRequired (must exist in the container; value >0 signed)
        -> 'heroPointChapter%02dRequirements' on the current flow's name (sub_82218CC8)
        -> CurrentXP stat (FindElementByNameHash_sub_821F16B8, vt+40, fctidz, low 32 bits)
        unavailable when (u32)xp < (u32)requirement
      objectiveIndex, triggerIsRepeatable (sub_821E6DA8): won && !repeatable
      *(u8*)(*0x8296DF5C + 1) != 0
      (float)(u64)(u32)*(*0x8296DF50 + 4) <= flt_820013B8   (lwz/std/lfd/fcfid: a u32, never negative)
    The game short-circuits the last three; they are all pure reads, so every reason is listed."""
    m = r.m
    reasons, inputs = [], {}
    undefined = []
    deps = attrs.str(b"triggerDependencies")
    inputs["triggerDependencies"] = deps.decode("latin1") if deps is not None else None
    if deps:
        tested = []
        for d in tokenize_deps(deps):
            bit = r.objective_bit(d)
            tested.append([d, bit])
            if not bit:
                reasons.append("dependency %d not won" % d)
        inputs["deps_tested"] = tested
    if attrs.container():
        if attrs.exists(b"heroPointChapterRequired"):
            req_ch = attrs.int(b"heroPointChapterRequired")
            req_ch_s = req_ch - (1 << 32) if req_ch & 0x80000000 else req_ch
            inputs["heroPointChapterRequired"] = req_ch_s
            if req_ch_s > 0:
                name = b"heroPointChapter%02dRequirements" % req_ch_s
                cur = r.current_flow()
                if not cur:
                    undefined.append("no current flow: the game would read a flow name at VA 0xC")
                else:
                    req = r.flow_int_by_name(name, m.cstr(cur + FLOW_NAME))
                    inputs["xp_requirement"] = [name.decode(), req]
                    e = r.find_stat(b"CurrentXP")
                    if not e:
                        undefined.append("CurrentXP stat missing: the game would call through a null vtable")
                    else:
                        xp = r.stat_value(e)
                        xp32 = fctidz_low32(xp)
                        inputs["CurrentXP"] = xp
                        if xp32 < req:
                            reasons.append("CurrentXP %d < %s %d" % (xp32, name.decode(), req))
    idx = attrs.int(b"objectiveIndex")
    rep = attrs.int(b"triggerIsRepeatable")
    inputs["objectiveIndex"], inputs["triggerIsRepeatable"] = idx, rep
    won = r.objective_bit(idx)
    inputs["won"] = won
    if won and rep == 0:
        reasons.append("objective %d already won and not repeatable" % idx)
    om = m.u32(G_OBJ_MGR)
    if not om:
        undefined.append("objective manager *0x8296DF5C is null")
    else:
        q = m.u8(om + 1)
        inputs["quitToFrontEnd"] = q
        if q:
            reasons.append("quit-to-front-end flag set")
    metro = m.u32(G_METRO)
    if not metro:
        undefined.append("metro object *0x8296DF50 is null")
    else:
        h = m.u32(metro + METRO_HEALTH)
        thr = m.f32(F_ZERO_820013B8)
        hf = struct.unpack(">f", struct.pack(">f", float(h)))[0]  # fcfid then frsp
        inputs["metroHealth"] = h
        if not (hf > thr):
            reasons.append("metro health %d <= %s" % (h, thr))
    avail = None if undefined else not reasons
    return {"available": avail, "reasons": reasons, "undefined": undefined, "inputs": inputs}


# ================================================================ snapshot ====
def _h(v):
    return None if v is None else "0x%08X" % v


class Snap:
    def __init__(self, img):
        self.r = GameReader(img)
        self.m = self.r.m
        self.checks = []

    def check(self, name, ok, detail=""):
        self.checks.append({"name": name, "ok": bool(ok), "detail": detail})
        return ok

    def guard(self, fn, what):
        try:
            return fn()
        except (ReadError, Unknown) as ex:
            self.check(what, False, str(ex))
            return None

    # ---- sections ----
    def fingerprint(self):
        m = self.m
        mz = self.guard(lambda: m.raw(0x82000000, 2) == b"MZ", "image MZ at 0x82000000")
        self.check("image MZ at 0x82000000", mz)
        for name, va, want in FINGERPRINT:
            got = self.guard(lambda va=va: m.u32(va), name)
            self.check(name, got == want, "got %s" % _h(got))

    def node_record(self, n):
        m, r = self.m, self.r
        k, v, fl = m.u32(n + NODE_KEY), m.u32(n + NODE_VALUE), m.u32(n + NODE_FLAGS)
        t = (fl >> 17) & 0x7F
        name = r.atom(k)
        rec = {"key": _h(k), "name": name.decode("latin1") if name is not None else None,
               "type": DAS_TYPE_NAMES.get(t, "t%d" % t), "flags": _h(fl), "raw": _h(v)}
        if t == 1:
            rec["value"] = struct.unpack(">f", struct.pack(">I", v))[0]
        elif t == 6:
            s = r.atom(v)
            rec["value"] = s.decode("latin1") if s is not None else None
        elif t == 3:
            rec["value"] = None
            vec = self.guard(lambda: m.raw(v, 12), "vector node %s" % rec["name"]) if v else None
            if vec:
                rec["value"] = list(struct.unpack(">3f", vec))
        else:
            rec["value"] = v
        if name is not None and fnv1(name) != k:
            self.check("DAS key == FNV-1(atom name) for %s" % rec["name"], False,
                       "key %s fnv %s" % (_h(k), _h(fnv1(name))))
        return rec

    def das_nodes(self, handle):
        m = self.m
        c = m.u32(handle + DAS_CONTAINER)
        b, e = m.u32(c), m.u32(c + 4)
        n = ((e - b) & 0xFFFFFFFF) >> 2
        if n > 20000:
            raise ReadError("DAS container %s count %d implausible" % (_h(c), n))
        nodes = [m.u32(b + 4 * i) for i in range(n)]
        keys = [m.u32(x) for x in nodes]
        self.check("DAS container %s sorted by key (binary search precondition)" % _h(c),
                   keys == sorted(keys) and len(set(keys)) == len(keys))
        return nodes

    def flows(self):
        m, r = self.m, self.r
        mgr = r.flow_mgr()
        out = {"mgr": _h(mgr)}
        if not mgr:
            self.check("world-flow manager *0x8296DF74 non-null", False)
            return out, 0
        out["current_def_index"] = m.u32(mgr + WFM_CUR_DEF)
        cdn = r.current_def_name(mgr)
        out["current_def_name"] = cdn.decode("latin1") if cdn is not None else None
        slots = []
        sl = m.u32(mgr + WFM_SLOTS)
        for i in range(8):
            f = m.u32(sl + 4 * i)
            rec = {"slot": i, "flow": _h(f)}
            if f:
                nm = m.cstr(f + FLOW_NAME)
                rec["name"] = nm.decode("latin1")
                rec["das"] = _h(m.u32(f + FLOW_DAS))
                ok = m.u32(f + FLOW_NAME_HASH) == hash31(nm)
                rec["name_hash_ok"] = ok
                self.check("flow slot %d name hash (+0x4C) == hash31(name)" % i, ok, rec["name"])
            slots.append(rec)
        out["slots"] = slots
        out["loaded"] = [s["name"] for s in slots if s.get("name")]
        cur = r.current_flow()
        out["current_flow"] = _h(cur)
        out["current_flow_name"] = m.cstr(cur + FLOW_NAME).decode("latin1") if cur else None
        self.check("current flow found (FindEntryByNameIn8Slot)", bool(cur), str(out["current_def_name"]))
        return out, cur

    def base_das(self, cur):
        m, r = self.m, self.r
        if not cur:
            return None
        handle = m.u32(cur + FLOW_DAS)
        nodes = self.das_nodes(handle)
        recs = [self.node_record(n) for n in nodes]
        by = {x["name"]: x for x in recs if x["name"] is not None}
        unnamed = sum(1 for x in recs if x["name"] is None)
        self.check("every current-flow DAS key resolves through the atom map", unnamed == 0,
                   "%d unnamed" % unnamed)
        # the saved subset: type-bit-16 vector (sub_82181E28 / sub_821E6D68)
        pv = handle + DAS_TYPEVEC + 20 * 4
        pb, pe = m.u32(pv), m.u32(pv + 4)
        pn = ((pe - pb) & 0xFFFFFFFF) >> 2
        persistent = [m.u32(pb + 4 * i) for i in range(min(pn, 20000))]
        pset = set(persistent)
        flag_set = {n for n, x in zip(nodes, recs) if int(x["flags"], 16) & 0x10000000}
        self.check("persistent vector == DAS nodes with flag 0x10000000 [data]", pset == flag_set,
                   "vector %d, flagged %d, sym-diff %d" % (len(pset), len(flag_set), len(pset ^ flag_set)))
        for n, x in zip(nodes, recs):
            x["persistent"] = n in pset
        return {"handle": _h(handle), "count": len(recs), "nodes": recs, "_by": by,
                "_persistent_nodes": persistent}

    def progress(self, das):
        by = das["_by"]
        out = {}

        def words(prefix):
            w = []
            for i in range(9):
                x = by.get("%s%d" % (prefix, i))
                w.append(x["value"] if x else None)
            return w
        st, fd = words("objectiveStatus"), words("objectiveFound")
        out["objectiveStatus"] = [_h(v) if v is not None else None for v in st]
        out["objectiveFound"] = [_h(v) if v is not None else None for v in fd]
        out["won"] = [32 * i + b for i, v in enumerate(st) if v is not None for b in range(32) if (v >> b) & 1]
        out["found"] = [32 * i + b for i, v in enumerate(fd) if v is not None for b in range(32) if (v >> b) & 1]
        self.check("objectiveStatus0..8 all present", None not in st)
        self.check("objectiveFound0..8 all present", None not in fd)
        num = by.get("numObjectives")
        out["numObjectives"] = num["value"] if num else None
        for f in HEADLINE_BASE_FIELDS:
            x = by.get(f)
            out[f] = x["value"] if x else None
        out["chapterRequirements"] = {k[len("heroPointChapter"):len("heroPointChapter") + 2]: v["value"]
                                      for k, v in sorted(by.items())
                                      if k.startswith("heroPointChapter") and k.endswith("Requirements")}
        out["chapterTitles"] = {k[len("chapter"):len("chapter") + 2]: v["value"] for k, v in sorted(by.items())
                                if k.startswith("chapter") and k.endswith("Title")}
        out["miniEventWeights"] = {k: v["value"] for k, v in sorted(by.items())
                                   if k.startswith("ME") and k.endswith("Weight")}
        ch = out.get("currentHeroPointChapter")
        if isinstance(ch, int) and out.get("numberOfHeroPointChapters") is not None:
            self.check("1 <= currentHeroPointChapter <= numberOfHeroPointChapters",
                       1 <= ch <= out["numberOfHeroPointChapters"], "%s/%s" % (ch, out["numberOfHeroPointChapters"]))
        reqs = [out["chapterRequirements"][k] for k in sorted(out["chapterRequirements"])]
        self.check("chapter requirements non-decreasing", all(a <= b for a, b in zip(reqs, reqs[1:])), str(reqs[:6]))
        # cross-check the ported bit test against the DAS words it reads
        mism = [b for b in range(9 * 32) if self.r.objective_bit(b) != (b in out["won"])]
        self.check("objective_bit() (port of sub_82224EE0) agrees with objectiveStatus words", not mism,
                   str(mism[:8]))
        return out

    def obj_mgr(self):
        m = self.m
        om = m.u32(G_OBJ_MGR)
        out = {"va": _h(om)}
        if om:
            raw = m.raw(om, 7)
            out["raw"] = raw.hex()
            for off, nm in OBJMGR_FIELDS.items():
                out[nm] = raw[off]
            self.check("objective manager bytes are 0/1", all(b in (0, 1) for b in raw[:7:2] + raw[1:2]), raw.hex())
        return out

    def metro(self, das_by, in_metropolis=True):
        m = self.m
        mo = m.u32(G_METRO)
        out = {"va": _h(mo), "in_metropolis": in_metropolis}
        if mo:
            out["health"] = m.u32(mo + METRO_HEALTH)
            out["max"] = m.u32(mo + METRO_MAX)
            out["percent"] = m.f32(mo + METRO_PERCENT)
            dm = das_by.get("MaxMetropolisHealth") if das_by else None
            out["das_MaxMetropolisHealth"] = dm["value"] if dm else None
            self.check("0 <= metro health <= metro max", 0 <= out["health"] <= out["max"],
                       "%d/%d" % (out["health"], out["max"]))
            if dm and not in_metropolis:
                # open item 17 (live run 1): outside Metropolis (the Warworld arena) the runtime
                # metro object is the arena's (max 10000 vs MaxMetropolisHealth 25000); the DAS
                # node describes Metropolis, so the comparison does not apply there
                out["max_check"] = "n/a: MetropolisWorldFlow is not loaded"
            elif dm:
                # the node's type is image data: anything but an int node is a failed check,
                # never a crash (verifier mutation M13, iter-463)
                self.check("metro max (runtime) == DAS MaxMetropolisHealth",
                           dm["type"] == "int" and out["max"] == dm["value"],
                           "%d vs %r (%s node)" % (out["max"], dm["value"], dm["type"]))
        return out

    def stats(self):
        m, r = self.m, self.r
        out = []
        for e in r.stat_list():
            nm = m.cstr(m.u32(e + STAT_NAME))
            fl = m.u8(e + STAT_FLAGS)
            val = self.guard(lambda e=e: r.stat_value(e), "stat value")
            rec = {"va": _h(e), "name": nm.decode("latin1") if nm is not None else None,
                   "flags": fl, "saved": bool(fl & 4), "active": fl & 1, "value": val}
            cap = self.guard(lambda e=e: decode_stat_cap(m, e), "stat cap (%s)" % rec["name"])
            if cap is not None:
                rec["cap"], rec["cap_offset"], rec["cap_setter"] = cap[0], cap[1], _h(cap[2])
                if cap[1] is not None:
                    # second observer: the class's AddToStat (vt+12) must clamp at the same offset
                    # (0x82204470: ... lfd f13,D(r3) ... stfd f13,8(r3))
                    add = self.guard(lambda e=e: m.raw(m.u32(m.u32(e) + STAT_ADD_VTSLOT), 32),
                                     "stat add slot (%s)" % rec["name"])
                    ws = struct.unpack(">8I", add) if add else ()
                    self.check("%s: AddToStat (vt+12) clamps at the SetStatCap offset +0x%X" % (rec["name"], cap[1]),
                               0xC9A30000 | cap[1] in ws, " ".join("%08X" % x for x in ws))
            out.append(rec)
        names = [s["name"] for s in out]
        self.check("stat names unique", len(set(names)) == len(names))
        return out

    def powerups(self):
        m = self.m
        pl = m.u32(G_POWERUPS)
        out = []
        if not pl:
            return out
        arr, n = m.u32(pl + PU_ELEMS), m.u32(pl + PU_COUNT)
        if n > 4096:
            raise ReadError("powerup count implausible")
        for i in range(n):
            e = m.u32(arr + 4 * i)
            nm = m.cstr(m.u32(e + PU_NAME))
            out.append({"va": _h(e), "name": nm.decode("latin1") if nm is not None else None,
                        "purchased": m.u8(e + PU_PURCHASED), "level_byte34": m.u8(e + PU_LEVEL)})
        self.check("powerup purchased bytes are 0/1", all(p["purchased"] in (0, 1) for p in out))
        return out

    @staticmethod
    def powerup_levels(pus):
        fam = {}
        for p in pus:
            nm = p["name"] or ""
            base = nm.split("Level")[0] if "Level" in nm else nm.replace("Infinite", "")
            fam.setdefault(base, 0)
            if p["purchased"]:
                fam[base] = max(fam[base], p["level_byte34"])
        return fam

    def save_projection(self, stats, pus, das):
        """What SaveMgr_CaptureAutoSave (AptClipProcessFrame_sub_8218E3A8) would write now, minus
        the options block: stats with flag&4 {hash31(name), f64} (sub_82165198), powerups
        {hash31(name), byte+35} (sub_82165250), current-flow persistent DAS nodes {key, u32 in
        the high word} with objectiveStatus4 repacked (sub_82181E28 / sub_82177310 /
        PackObjectiveStatusBits_sub_82165638), and the metro record (sub_82186F70, see
        metro_save_value)."""
        m, r = self.m, self.r
        proj = {"stats": [], "powerups": [], "flow_persistent": []}
        for s in stats:
            if s["saved"] and s["name"] is not None:
                proj["stats"].append({"key": _h(hash31(s["name"].encode("latin1"))), "name": s["name"],
                                      "value": s["value"]})
        for p in pus:
            if p["name"] is not None:
                proj["powerups"].append({"key": _h(hash31(p["name"].encode("latin1"))), "name": p["name"],
                                         "byte": p["purchased"]})
        if das:
            for n in das["_persistent_nodes"]:
                k, v, fl = m.u32(n), m.u32(n + 4), m.u32(n + 8)
                t = (fl >> 17) & 0x7F
                nm = r.atom(k)
                val = v if t in (0, 1, 5) else 0
                rec = {"key": _h(k), "name": nm.decode("latin1") if nm is not None else None,
                       "type": DAS_TYPE_NAMES.get(t, "t%d" % t), "hi_word": _h(val)}
                if nm == b"objectiveStatus4":
                    packed = val
                    for dest, src in PACK_OBJ4_PAIRS:
                        bit = 1 << (dest & 31)
                        packed = (packed | bit) if r.objective_bit(src) else (packed & ~bit & 0xFFFFFFFF)
                    rec["hi_word"] = _h(packed)
                    rec["repacked_from"] = _h(val)
                proj["flow_persistent"].append(rec)
        proj["metro"] = self.guard(self.metro_save_value, "metro save value")
        return proj

    def metro_save_value(self):
        """SaveRecords_WriteMetroHealth_sub_82186F70: record u32 = max_u32(health,
        low32(fctidz(f32(f32((double)(u32)max) * factor)))), factor = the 32 value bits of
        the current flow's MetroHealthSaveThreshold node returned by sub_82182020 (types 0/1/5;
        any other type or a missing node gives 0) and loaded with lfs, i.e. read as an f32."""
        m, r = self.m, self.r
        mo = m.u32(G_METRO)
        if not mo:
            raise Unknown("metro object *0x8296DF50 is null: the writer would fault")
        h, mx = m.u32(mo + METRO_HEALTH), m.u32(mo + METRO_MAX)
        n = r.current_flow_attr(b"MetroHealthSaveThreshold")      # AptResolveObjectPathValue_sub_82177660
        bits = 0
        if n and ((m.u32(n + NODE_FLAGS) >> 17) & 0x7F) in (0, 1, 5):
            bits = m.u32(n + NODE_VALUE)
        factor = struct.unpack(">f", struct.pack(">I", bits))[0]
        f32 = lambda x: struct.unpack(">f", struct.pack(">f", x))[0]   # noqa: E731 (frsp / fmuls)
        prod = f32(f32(float(mx)) * factor) if factor == factor else factor
        floor = fctidz_low32(prod)
        return {"health": h, "max": mx, "threshold_bits": _h(bits), "threshold": factor,
                "floor": floor, "value": h if h > floor else floor}

    def triggers(self):
        m, r = self.m, self.r
        reg = m.u32(G_FILTER_REGISTRY)
        out = {"registry": _h(reg), "members": []}
        if not reg:
            self.check("filter registry *0x8296DD08 non-null", False)
            return out
        nm_filter = m.cstr(m.u32(OFF_NAME_OBJ_TRIGGERS))
        nm_index = m.cstr(m.u32(OFF_NAME_OBJECTIVE_INDEX))
        out["filter_name"] = nm_filter.decode("latin1")
        self.check("off_82912090[0] == 'OBJ|TRIGGERS'", nm_filter == b"OBJ|TRIGGERS", str(nm_filter))
        self.check("off_829120BC[0] == 'objectiveIndex'", nm_index == b"objectiveIndex", str(nm_index))
        nametbl = m.u32(reg)
        mask = r.build_mask(nametbl, nm_filter)
        engaged_mask = r.build_mask(nametbl, b"ENGAGED")
        out["mask"] = mask.hex()
        out["engaged_mask"] = engaged_mask.hex()
        self.check("ENGAGED resolves to a real row (non-default)",
                   r.hash_lookup(nametbl + 84, b"ENGAGED") != 0)
        filt = r.find_filter(m.u32(reg + 8), mask)
        out["filter"] = _h(filt)
        if not filt:
            self.check("OBJ|TRIGGERS filter found", False)
            return out
        for mem in r.filter_members(filt):
            rec = {"member": _h(mem)}
            out["members"].append(rec)
            if not mem:
                continue
            actor = self.guard(lambda mem=mem: r.get_component(mem, FOURCC_ACTR), "member vt+36('ACTR')")
            rec["actor"] = _h(actor)
            if not actor:
                continue
            handle = m.u32(actor + ACTOR_DAS)
            rec["das"] = _h(handle)
            groups = m.raw(actor + ACTOR_GROUPS, 32)
            rec["groups"] = groups.hex()
            rec["in_OBJ_TRIGGERS"] = all((g & k) == k for g, k in zip(groups, mask))
            rec["in_ENGAGED"] = any(e for e in engaged_mask) and all((g & k) == k for g, k in zip(groups, engaged_mask))
            self.check("trigger %s carries the OBJ|TRIGGERS bits" % _h(actor), rec["in_OBJ_TRIGGERS"])
            attrs = {}
            if handle:
                for n in self.das_nodes(handle):
                    x = self.node_record(n)
                    attrs[x["name"] or x["key"]] = x["value"]
            rec["attrs"] = attrs
            rec["objectiveIndex"] = attrs.get("objectiveIndex")
            rec["objectiveWorldFlowName"] = attrs.get("objectiveWorldFlowName")
            rec["objectiveAvailable_attr"] = attrs.get("objectiveAvailable")
            rec["fsm"] = self.guard(lambda actor=actor, attrs=attrs, handle=handle:
                                    self.trigger_fsm(actor, attrs, handle), "trigger FSM")
            rec["power_mgr"] = _h(m.u32(actor + ACTOR_POWERS))
            if m.u32(actor + ACTOR_POWERS):
                pw = self.guard(lambda actor=actor: r.power_find(m.u32(actor + ACTOR_POWERS), b"TriggerGate"),
                                "TriggerGate lookup")
                rec["TriggerGate"] = {"power": _h(pw), "flags8": m.u8(pw + 8), "stage9": m.u8(pw + 9)} if pw else None
            rec["verdict"] = self.guard(lambda handle=handle: availability(r, LiveAttrs(r, handle)),
                                        "availability (live)")
        return out

    def trigger_fsm(self, actor, attrs, handle=0):
        """actor+0x84 brain (brain+0x3C == actor), brain+0x1C machine, *(machine) current state.
        These three offsets are [data]; each is guarded: the state must carry the trigger-state
        vtable 0x82017790 (slots = sub_822737D0/sub_82273B40) and its +0x14 must equal the
        trigger's triggerDistance (sub_822737D0 stores it there). state+0x18 is linked to the
        triggerEvent attribute the same way: the enter action stores sub_8229A568(triggerEvent)
        when the string is non-empty, else 0."""
        m, r = self.m, self.r
        brain = m.u32(actor + ACTOR_BRAIN)
        if not brain or m.u32(brain + BRAIN_ACTOR) != actor:
            return {"ok": False, "why": "actor+0x84 is not a component pointing back at the actor"}
        mach = m.u32(brain + BRAIN_MACHINE)
        st = m.u32(mach) if mach else 0
        vt = m.u32(st) if st else 0
        rec = {"brain": _h(brain), "machine": _h(mach), "state": _h(st), "state_vt": _h(vt)}
        if vt != VT_TRIGGER_STATE:
            rec.update(ok=False, why="current state is not the trigger state class")
            return rec
        fl = m.u32(st + STATE_FLAGS)
        dist = m.f32(st + STATE_DISTANCE)
        rec.update(ok=True, flags=_h(fl), disabled_bit0=fl & 1, distance=dist,
                   entity_id=m.u32(st + STATE_ENTITY_ID))
        td = attrs.get("triggerDistance")
        if td is not None:
            rec["distance_matches_attr"] = abs(dist - td) <= 1e-3 * max(1.0, abs(td))
            self.check("trigger FSM +0x14 == triggerDistance (%s)" % _h(actor), rec["distance_matches_attr"],
                       "%s vs %s" % (dist, td))
        # triggerEvent -> state+0x18 (sub_822737D0: sub_821E6F30 atom, strlen, sub_8229A568)
        ev = m.u32(st + STATE_EVENT)
        name = r.das_str(handle, b"triggerEvent") if handle else None
        want = input_event_id(m, name) if name else 0
        rec.update(event_id=ev, triggerEvent=name.decode("latin1") if name is not None else None,
                   event_expected=want, event_matches_attr=ev == want)
        self.check("trigger FSM +0x18 == sub_8229A568(triggerEvent) (%s)" % _h(actor), ev == want,
                   "%d vs %d (%r)" % (ev, want, name))
        return rec

    def flow_fsms(self, flows, graphs):
        """Every loaded flow's FSM (flow+0x6C, ticked by WorldFlow_Update_sub_82225E60). The
        current state index is fsm+8 (-1 = not running); it is mapped to the graph's state id
        when a decoded graph of the flow's name is supplied, and the mapping is proven on the
        image: every live state's transition list (dest indices, FsmWalkTransitions_sub_8278C448)
        must equal the decoded graph's transitions with ids mapped to document-order indices."""
        m = self.m
        out = []
        for sl in (flows or {}).get("slots", []):
            if not sl.get("name"):
                continue
            f = int(sl["flow"], 16)
            fsm = m.u32(f + FLOW_FSM)
            rec = {"flow": sl["name"], "fsm": _h(fsm)}
            out.append(rec)
            if not fsm:
                continue
            vt = m.u32(fsm)
            rec["vt"] = _h(vt)
            if vt != VT_FLOW_FSM:
                rec["ok"] = False
                self.check("flow FSM %s carries the flow-FSM vtable 0x8200E5E4" % sl["name"], False, _h(vt))
                continue

            def s32(v):
                return v - (1 << 32) if v & 0x80000000 else v
            cur, prev, start = s32(m.u32(fsm + FSM_CUR)), s32(m.u32(fsm + FSM_PREV)), s32(m.u32(fsm + FSM_START))
            n, arr = m.u32(fsm + FSM_COUNT), m.u32(fsm + FSM_STATES)
            if n > 4096:
                raise ReadError("flow FSM %s state count %d implausible" % (sl["name"], n))
            rec.update(ok=True, index=cur, prev_index=prev, start_index=start, count=n, states=_h(arr))
            in_range = cur == -1 or (arr != 0 and 0 <= cur < n)
            self.check("flow FSM %s current index in [-1, count)" % sl["name"], in_range, "%d/%d" % (cur, n))
            if in_range and cur >= 0:
                st = m.u32(arr + 4 * cur)
                rec["state"], rec["state_vt"] = _h(st), _h(m.u32(st))
            g = (graphs or {}).get(sl["name"])
            if g is None:
                rec["graph"] = None
                continue
            ids = fsm_state_ids(g)
            rec["graph"] = g.get("file") or sl["name"]
            if len(ids) != n:
                rec["graph_verified"] = False
                self.check("flow FSM %s state count == decoded graph" % sl["name"], False, "%d vs %d" % (n, len(ids)))
                continue
            idx = {v: i for i, v in enumerate(ids)}
            bad = []
            for i in range(n):
                s = m.u32(arr + 4 * i)
                tn, ta = m.u32(s + FSTATE_NTRANS), m.u32(s + FSTATE_TRANS)
                if tn > 256:
                    raise ReadError("state %d transition count %d implausible" % (i, tn))
                live = [m.u32(m.u32(ta + 4 * k) + FTRANS_DEST) for k in range(tn)]
                want = [idx.get(int(t["dest"])) if t.get("dest") is not None else None
                        for t in g["states"][i]["transitions"]]
                if live != want:
                    bad.append(i)
            rec["graph_verified"] = not bad
            rec["transitions_checked"] = n
            self.check("flow FSM %s: every live transition list == the decoded graph (%d states)" % (sl["name"], n),
                       not bad, "mismatched state indices %s" % bad[:8])
            if bad:
                continue
            rec["state_id"] = ids[cur] if in_range and cur >= 0 else None
            rec["prev_state_id"] = ids[prev] if 0 <= prev < n else None
            rec["start_state_id"] = ids[start] if 0 <= start < n else None
            self.check("flow FSM %s start index == the graph's start_state" % sl["name"],
                       g.get("start") is None or rec["start_state_id"] == int(g["start"]),
                       "%s vs %s" % (rec["start_state_id"], g.get("start")))
            if in_range and cur >= 0:
                rec["state_kind"] = g["states"][cur].get("kind")
                rec["state_script"] = g["states"][cur].get("script")
        return out

    def static_verdicts(self, docs_paths):
        r = self.r
        out = []
        for p in docs_paths:
            if not os.path.isfile(p):
                continue
            with open(p, encoding="utf-8") as f:
                docs = json.load(f)
            for d in docs:
                a = d.get("attrs", {})
                if "objectiveIndex" not in a or "triggerDependencies" not in a:
                    continue
                v = self.guard(lambda a=a, d=d: availability(r, StaticAttrs(a, d.get("types", {}))),
                               "availability (static %s)" % d.get("at"))
                out.append({"source": "static:%s@%s" % (os.path.basename(p), d.get("at")),
                            "objectiveIndex": int(a["objectiveIndex"]),
                            "objectiveWorldFlowName": a.get("objectiveWorldFlowName"),
                            "heroPointChapterRequired": a.get("heroPointChapterRequired"),
                            "verdict": v})
        return out


def snapshot(img, trigger_docs=None, fsm_graphs=None):
    """fsm_graphs: {graph name: worldflow_fsm model} used to map flow-FSM indices to state ids
    (load_fsm_graphs(DEFAULT_AST)); None/{} reports the indices only."""
    s = Snap(img)
    out = {"schema": SCHEMA, "image": img.describe(), "image_layer": IMAGE_LAYER}
    s.fingerprint()
    flows = s.guard(lambda: s.flows(), "flows")
    flows, cur = flows if flows else ({}, 0)
    out["flows"] = flows
    fsms = s.guard(lambda: s.flow_fsms(flows, fsm_graphs), "flow FSMs") or []
    out["flow_fsms"] = fsms
    base = next((x for x in fsms if x["flow"] == "BaseWorldFlow"), None)
    out["base_fsm"] = ({k: base.get(k) for k in ("index", "state_id", "prev_index", "prev_state_id",
                                                  "count", "state_kind", "graph_verified")} if base else None)
    das = s.guard(lambda: s.base_das(cur), "current-flow DAS") if cur else None
    out["progress"] = s.guard(lambda: s.progress(das), "progress") if das else None
    out["objective_mgr"] = s.guard(s.obj_mgr, "objective manager")
    in_metro = "MetropolisWorldFlow" in ((flows or {}).get("loaded") or [])
    out["metro"] = s.guard(lambda: s.metro(das["_by"] if das else None, in_metro), "metro")
    # the objective-failure marker (live run 1, open item 15): see base_fail_states()
    bfs = base_fail_states((fsm_graphs or {}).get("BaseWorldFlow"))
    by = (das or {}).get("_by") or {}
    act_n, mini_n = by.get("objectiveActivated"), by.get("miniEventEngaged")
    out["failure"] = failure_verdict(
        (out["base_fsm"] or {}).get("state_id") if (out["base_fsm"] or {}).get("graph_verified") else None,
        act_n["value"] if act_n else None, mini_n["value"] if mini_n else None, bfs)
    out["failure"]["graph_sets"] = None if bfs[0] is None else {"objective_loss": bfs[0], "game_over": bfs[1]}
    stats = s.guard(s.stats, "stats") or []
    out["stats"] = stats
    out["xp"] = {k: next((x["value"] for x in stats if x["name"] == k), None)
                 for k in ("CurrentXP", "PreviousXP", "NextXP")}
    cxp = next((x for x in stats if x["name"] == "CurrentXP"), None)
    out["xp"]["CurrentXP_cap"] = cxp.get("cap") if cxp else None
    pr = out["progress"] or {}
    ch, nch = pr.get("currentHeroPointChapter"), pr.get("numberOfHeroPointChapters")
    if cxp and cxp.get("cap") and isinstance(ch, int) and isinstance(nch, int) and ch < nch:
        # triangulation with the game's own Lua (BaseWorldFlow state 105): after every pass it
        # ends with SetStatCap("CurrentXP", heroPointChapter<current+1>Requirements); a cap of 0
        # means state 105 has not run yet (pre-game), so there is nothing to compare
        req = pr.get("chapterRequirements", {}).get("%02d" % (ch + 1))
        out["xp"]["cap_expected"] = req
        s.check("CurrentXP cap == heroPointChapter%02dRequirements (state 105 Lua)" % (ch + 1),
                req is not None and float(req) == cxp["cap"], "%s vs %s" % (cxp["cap"], req))
    pus = s.guard(s.powerups, "powerups") or []
    out["powerups"] = pus
    out["powerup_levels"] = Snap.powerup_levels(pus)
    out["save_projection"] = s.guard(lambda: s.save_projection(stats, pus, das), "save projection")
    out["triggers"] = s.guard(s.triggers, "triggers")
    verdicts = []
    for mem in (out["triggers"] or {}).get("members", []):
        if "verdict" in mem:
            v = mem["verdict"]
            verdicts.append({"source": "live:%s" % mem.get("actor"), "objectiveIndex": mem.get("objectiveIndex"),
                             "objectiveWorldFlowName": mem.get("objectiveWorldFlowName"), "verdict": v,
                             "game_objectiveAvailable": mem.get("objectiveAvailable_attr"),
                             "fsm_disabled_bit0": (mem.get("fsm") or {}).get("disabled_bit0")})
            # triangulation: the game's own enter action wrote objectiveAvailable:=0 and state|=1
            # when it judged the trigger unavailable (it never writes 1; the doc default is 1)
            if v and v["available"] is False and out["failure"]["kind"] is not None:
                # the game's objectiveAvailable is the verdict of the trigger's last enter action
                # (state 105); a loss changes its inputs afterwards (metro health 0 in live run 1's
                # MSTobj02 failure) and no enter runs until Retry / 105, so the cross-check does
                # not apply in a loss state (open item 17, verifier)
                v["triangulation"] = "n/a: base FSM in loss state %s (%s)" % (
                    out["failure"]["state_id"], out["failure"]["kind"])
            elif v and v["available"] is False:
                s.check("live verdict 'unavailable' matches the game's objectiveAvailable==0 (%s)" % mem.get("actor"),
                        mem.get("objectiveAvailable_attr") == 0, str(mem.get("objectiveAvailable_attr")))
    if trigger_docs:
        verdicts += s.static_verdicts(trigger_docs)
    out["availability"] = verdicts
    if das:
        das.pop("_by")
        das.pop("_persistent_nodes")
    out["current_flow_das"] = das
    out["objective_fsm_current"] = s.guard(lambda: _h(s.m.u32(G_FSM_CURRENT)), "fsm current")
    out["reads"] = s.m.reads
    led = s.m.ledger()
    out["read_ledger"] = {"pages": len(s.m.pages), "ranges": len(led),
                          "bytes": sum(e - b for b, e in led)}
    out["_ledger"] = led
    out["checks"] = s.checks
    out["checks_failed"] = sum(1 for c in s.checks if not c["ok"])
    return out


# ================================================================ reporting ====
def summary_text(snap):
    L = []
    img = snap.get("image", {})
    L.append("objstate %s  image=%s %s" % (snap.get("schema"), img.get("kind"), img.get("path", "")))
    fl = snap.get("flows") or {}
    L.append("flows: current=%s loaded=%s" % (fl.get("current_flow_name"), fl.get("loaded")))
    pr = snap.get("progress") or {}
    if pr:
        L.append("chapter %s/%s  GoToNextChapter=%s  numObjectives=%s" % (
            pr.get("currentHeroPointChapter"), pr.get("numberOfHeroPointChapters"),
            pr.get("GoToNextChapter"), pr.get("numObjectives")))
        L.append("won bits: %s" % pr.get("won"))
        L.append("found bits: %s" % pr.get("found"))
        L.append("gates: triggersAvailable=%s waitingForLoad=%s objectiveActivated=%s triggerEngaged=%s" % (
            pr.get("triggersAvailable"), pr.get("waitingForLoad"), pr.get("objectiveActivated"),
            pr.get("triggerEngaged")))
        L.append("scratch: objectiveWorldFlowName=%s objectiveBitIndex=%s complete=%s won=%s" % (
            pr.get("objectiveWorldFlowName"), pr.get("objectiveBitIndex"), pr.get("objectiveComplete"),
            pr.get("objectiveWon")))
    bf = snap.get("base_fsm") or {}
    if bf:
        L.append("BaseWorldFlow FSM: index %s = state %s (%s)  prev %s = %s  graph_verified=%s" % (
            bf.get("index"), bf.get("state_id"), bf.get("state_kind"), bf.get("prev_index"),
            bf.get("prev_state_id"), bf.get("graph_verified")))
    fa = snap.get("failure") or {}
    if fa.get("kind"):
        L.append("FAILURE: base state %s is a %s state; objective_failed=%s" % (
            fa.get("state_id"), fa.get("kind"), fa.get("objective_failed")))
    others = [x for x in snap.get("flow_fsms", []) if x.get("flow") != "BaseWorldFlow" and x.get("ok")]
    if others:
        L.append("other flow FSMs (index/count): %s" % ", ".join(
            "%s %s/%s" % (x["flow"], x.get("index"), x.get("count")) for x in others))
    L.append("xp: %s" % snap.get("xp"))
    mt = snap.get("metro") or {}
    L.append("metro: health=%s max=%s pct=%s das_max=%s" % (mt.get("health"), mt.get("max"), mt.get("percent"),
                                                           mt.get("das_MaxMetropolisHealth")))
    L.append("objective mgr: %s" % {k: v for k, v in (snap.get("objective_mgr") or {}).items() if k != "va"})
    L.append("powerup levels: %s" % snap.get("powerup_levels"))
    tr = snap.get("triggers") or {}
    L.append("OBJ|TRIGGERS filter %s: %d member(s)" % (tr.get("filter"), len(tr.get("members", []))))
    for mem in tr.get("members", []):
        f = mem.get("fsm") or {}
        v = mem.get("verdict") or {}
        L.append("  idx=%s %-24s objectiveAvailable=%s ENGAGED=%s fsm.flags=%s event=%s verdict=%s %s" % (
            mem.get("objectiveIndex"), mem.get("objectiveWorldFlowName"), mem.get("objectiveAvailable_attr"),
            mem.get("in_ENGAGED"), f.get("flags"), f.get("event_id"), v.get("available"),
            "; ".join(v.get("reasons", []))))
    av = [a for a in snap.get("availability", []) if a["source"].startswith("static")]
    if av:
        ok = sorted(a["objectiveIndex"] for a in av if (a["verdict"] or {}).get("available"))
        L.append("static trigger docs: %d evaluated; available now: %s" % (len(av), ok))
    L.append("checks: %d, failed %d" % (len(snap.get("checks", [])), snap.get("checks_failed", 0)))
    for c in snap.get("checks", []):
        if not c["ok"]:
            L.append("  FAIL %s  %s" % (c["name"], c["detail"]))
    L.append("reads: %s" % snap.get("reads"))
    return "\n".join(L)


STORY_KEYS = ["currentHeroPointChapter", "GoToNextChapter", "triggersAvailable", "waitingForLoad",
              "objectiveActivated", "triggerEngaged", "objectiveWorldFlowName", "MaxMetropolisHealth",
              "minTimeBetweenTriggerSpawns", "maxTimeBetweenTriggerSpawns", "won", "found",
              "miniEventWeights"]


def diff(a, b):
    out = []
    pa, pb = a.get("progress") or {}, b.get("progress") or {}
    for k in STORY_KEYS:
        if pa.get(k) != pb.get(k):
            out.append("progress.%s: %s -> %s" % (k, pa.get(k), pb.get(k)))
    for k in ("CurrentXP", "PreviousXP", "NextXP", "CurrentXP_cap"):
        if (a.get("xp") or {}).get(k) != (b.get("xp") or {}).get(k):
            out.append("xp.%s: %s -> %s" % (k, (a.get("xp") or {}).get(k), (b.get("xp") or {}).get(k)))
    for k in ("index", "state_id"):
        if (a.get("base_fsm") or {}).get(k) != (b.get("base_fsm") or {}).get(k):
            out.append("base_fsm.%s: %s -> %s" % (k, (a.get("base_fsm") or {}).get(k), (b.get("base_fsm") or {}).get(k)))
    for k in ("health", "max"):
        if (a.get("metro") or {}).get(k) != (b.get("metro") or {}).get(k):
            out.append("metro.%s: %s -> %s" % (k, a["metro"].get(k), b["metro"].get(k)))
    if a.get("powerup_levels") != b.get("powerup_levels"):
        out.append("powerup_levels: %s -> %s" % (a.get("powerup_levels"), b.get("powerup_levels")))
    if (a.get("flows") or {}).get("loaded") != (b.get("flows") or {}).get("loaded"):
        out.append("flows.loaded: %s -> %s" % (a["flows"].get("loaded"), b["flows"].get("loaded")))

    def vmap(s):
        return {(x["source"].split(":")[0], x["objectiveIndex"], x.get("objectiveWorldFlowName")):
                (x["verdict"] or {}).get("available") for x in s.get("availability", [])}
    va, vb = vmap(a), vmap(b)
    for k in sorted(set(va) | set(vb), key=lambda t: (t[0], t[1] if t[1] is not None else -1, str(t[2]))):
        if va.get(k) != vb.get(k):
            out.append("availability %s idx=%s %s: %s -> %s" % (k[0], k[1], k[2], va.get(k), vb.get(k)))
    sa = {x["name"]: x["value"] for x in (a.get("save_projection") or {}).get("stats", [])}
    sb = {x["name"]: x["value"] for x in (b.get("save_projection") or {}).get("stats", [])}
    for k in sorted(set(sa) | set(sb)):
        if sa.get(k) != sb.get(k):
            out.append("saved stat %s: %s -> %s" % (k, sa.get(k), sb.get(k)))
    # the entity-byte records and the metro record are part of the save too (critique B4)
    ua = {x["name"]: x["byte"] for x in (a.get("save_projection") or {}).get("powerups", [])}
    ub = {x["name"]: x["byte"] for x in (b.get("save_projection") or {}).get("powerups", [])}
    for k in sorted(set(ua) | set(ub), key=str):
        if ua.get(k) != ub.get(k):
            out.append("saved entity byte %s: %s -> %s" % (k, ua.get(k), ub.get(k)))
    ma = ((a.get("save_projection") or {}).get("metro") or {}).get("value")
    mb = ((b.get("save_projection") or {}).get("metro") or {}).get("value")
    if ma != mb:
        out.append("saved metro record: %s -> %s" % (ma, mb))
    fa = {x["key"]: x["hi_word"] for x in (a.get("save_projection") or {}).get("flow_persistent", [])}
    fb = {x["key"]: x["hi_word"] for x in (b.get("save_projection") or {}).get("flow_persistent", [])}
    names = {x["key"]: x["name"] for s in (a, b) for x in (s.get("save_projection") or {}).get("flow_persistent", [])}
    for k in sorted(set(fa) | set(fb)):
        if fa.get(k) != fb.get(k):
            out.append("saved DAS %s (%s): %s -> %s" % (names.get(k), k, fa.get(k), fb.get(k)))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sp = sub.add_parser("snap")
    sp.add_argument("--image", required=True)
    sp.add_argument("--base", type=lambda x: int(x, 0))
    sp.add_argument("--label")
    sp.add_argument("--out", default=DEFAULT_OUT)
    sp.add_argument("--trigger-docs", default=None, help="comma list; default = archive/v1_devtools docs if present")
    sp.add_argument("--no-trigger-docs", action="store_true")
    sp.add_argument("--ledger", help="write the touched guest page ranges (JSON [[start,end],...]) here")
    sp.add_argument("--ast", default=DEFAULT_AST,
                    help="BGFA container whose FSM graphs name the flow-FSM states (default: retail baseworl.AST)")
    sp.add_argument("--no-ast", action="store_true", help="report flow-FSM indices only")
    sp.add_argument("--write-snap", help="write the touched guest pages of this image as a .snap "
                                         "(tools/guest_snap.py format; read back with --image snap:<file>)")
    dp = sub.add_parser("diff")
    dp.add_argument("a")
    dp.add_argument("b")
    sm = sub.add_parser("summary")
    sm.add_argument("a")
    args = ap.parse_args(argv)
    if args.cmd == "snap":
        img = open_image(args.image, base=args.base)
        docs = [] if args.no_trigger_docs else (args.trigger_docs.split(",") if args.trigger_docs
                                                 else DEFAULT_TRIGGER_DOCS)
        graphs = {} if args.no_ast else load_fsm_graphs(args.ast)
        snap = snapshot(img, docs, graphs)
        led = snap.pop("_ledger")
        if args.ledger:
            with open(args.ledger, "w", encoding="utf-8") as f:
                json.dump([["0x%08X" % b, "0x%08X" % e] for b, e in led], f)
        label = args.label or args.image.replace(":", "_").replace("/", "_").replace("\\", "_")
        if args.write_snap:
            import guest_snap
            info = guest_snap.write_snap(args.write_snap, img, guest_snap.pages_of_ranges(led),
                                         meta={"label": label, "reader": "objstate.py snap",
                                               "source_spec": args.image})
            print("wrote %s: %d pages (%d partial, %d unreadable), %d bytes" % (
                args.write_snap, info["pages"], info["partial"], len(info["missing"]), info["file_bytes"]))
        os.makedirs(args.out, exist_ok=True)
        jp = os.path.join(args.out, label + ".json")
        with open(jp, "w", encoding="utf-8") as f:
            json.dump(snap, f, indent=1)
        txt = summary_text(snap)
        with open(os.path.join(args.out, label + ".txt"), "w", encoding="utf-8") as f:
            f.write(txt + "\n")
        print(txt)
        print("wrote", jp)
        return 1 if snap["checks_failed"] else 0
    if args.cmd == "diff":
        with open(args.a, encoding="utf-8") as f:
            a = json.load(f)
        with open(args.b, encoding="utf-8") as f:
            b = json.load(f)
        d = diff(a, b)
        print("\n".join(d) if d else "no differences in the compared set")
        return 1 if d else 0
    if args.cmd == "summary":
        with open(args.a, encoding="utf-8") as f:
            print(summary_text(json.load(f)))
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
