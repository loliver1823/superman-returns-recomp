"""scanout_route.py - the first CAUSALLY REACHABLE scanout for ONE draw.

r2, rebuilt against the director's REWORK ruling (ledger 5830d40). The r1 router
was broader than the executor's real causal graph in five ways; each is corrected
below, and two of the corrections need data that DOES NOT EXIST in the trace, so
the production path REFUSES rather than guessing.

    python tools/scanout_route.py <trace> --record N [--admit admit.txt]
    python tools/scanout_route.py --self-test

WHY THIS TOOL EXISTS. A treatment must be measured at the first present its draw
can actually reach. `PINNED_PRESENT = 10` measured every treatment at a REPEAT
frame containing zero draws, so a draw executing in present 1 was graded nine
frames and five full redraws later, and a null result meant nothing.

THE ROUTE. A draw writes its bound render target; that surface reaches the screen
only through resolves. This walks the chain forward as a taint until a resolve
writes the FRONT key that the next Present scans out.

── the five corrections ──────────────────────────────────────────────────────

1. RT0 ONLY, GATED ON THE WRITE MASK. r1 tainted every bound MRT. The executor
   binds exactly one: `hleSubmitDraw` sets `NumRenderTargets = 1` and records
   `b.rt[0]` (d3d12_backend.cpp:5870, :4539). Colour taint is applied only when
   the RT0 write mask is nonzero -- `stateValid ? (colorMask & 0xF) : 0xF`,
   mirroring d3d12_backend.cpp:4258. `depthOnly` cannot zero it here because
   hleSubmitDraw passes `depthOnly=false` literally (:4539), so unlike the PM4
   path there is no RB_MODECONTROL term to model.

2. EXECUTED, NOT MERELY ELIGIBLE. `resolve_eligible()` mirrors only the guest-
   side rect/level/slice prefilter (hle_video.cpp:1917). The backend still
   refuses for a missing source, a depth gate, bad dimensions, a closed list, an
   allocation failure or slot exhaustion; a product exists only when
   `hleResolveToTexture` returns a NONZERO slot (hle_video.cpp:1946). That
   outcome is not in the trace. FAILS CLOSED -- see SIDECARS below.

3. KILL SEMANTICS. r1's taint sets only ever grew. Now: a provably FULL clear of
   a tainted surface REMOVES the taint (colour flags 0x0F, depth 0x10 -- Xbox
   360 semantics, d3d12_backend.hpp:332); and a successful full resolve ASSIGNS
   `dest_taint = src_taint`, so an untainted source OVERWRITES a stale
   destination. A clear that is not provably full over a tainted surface makes
   the route AMBIGUOUS and the tool refuses, because guessing either way can
   nominate a frame the draw does not reach.

4. A BOUND TEXTURE IS NOT A SAMPLED ONE. r1 treated any draw with a matching
   bound key as a consumer. The backend publishes all 16 sampler slots to the
   descriptor table unconditionally (d3d12_backend.cpp:4383), so a bind proves
   nothing about a read. A consumer must be ADMITTED (available: the admit map)
   AND its pixel shader must actually reference that sampler (NOT available --
   no reflection exists anywhere in gfx/). FAILS CLOSED.

5. ONE DRAW PER CALL. r1 unioned all targets and returned as soon as ANY reached
   a present, so it could answer before later manifest members were even seen.
   `route_one()` is singleton-only; `route_many()` groups equal sinks and
   REFUSES on differing ones rather than averaging them.

── SIDECARS THIS TOOL NEEDS AND DOES NOT HAVE ───────────────────────────────

  resolve-execution   per-resolve record -> did hleResolveToTexture return a
                      nonzero slot. The provenance ring already computes exactly
                      this field (`recorded = slot != 0`, hle_prov_ring) but it
                      is trigger-armed and capped at 16 detail rows per frame /
                      24 destinations -- not a per-record sidecar for a 52,466-
                      record replay. Needs an SR_DRAW_ADMIT_MAP analogue.

  ps-sampler-refs     pixel-shader hash -> the set of sampler indices its DXIL
                      actually reads. Nothing in the tree reflects this today.

BOTH NOW EXIST (iter-457): `SR_RESOLVE_EXEC_MAP` writes the first and
`tools/gen_ps_sampler_map.py` derives the second offline from the DXIL
cache. Supply them via --resolve-exec / --ps-samplers; without them the
tool still refuses BY NAME rather than assuming either.
Read-only. No replay, no rebuild, no guest writes.
"""
import argparse, struct, sys

PRESENT, CLEAR, CLEARF, SET_RT, SET_DS = 0, 1, 2, 3, 4
SET_TEX, RESOLVE, SET_SHADERS, SET_RS = 9, 20, 15, 19
DRAW_TYPES = {10, 11, 21}

# Xbox 360 clear flags -- NOT PC D3D9. Mirrors d3d12_backend.hpp:332.
CLEAR_COLOR, CLEAR_DEPTH = 0x0F, 0x10
# Render-state block indices. Mirrors hle_video.cpp:517-520.
RS_COLORMASK_IDX = 0x2104 - 0x2100          # 4
RS_DEPTHCTL_IDX = 26 + (0x2200 - 0x2200)    # kRsRun2Base + 0 = 26
Z_WRITE_ENABLE = 0x4                        # RB_DEPTHCONTROL

SIDECAR_RESOLVE_EXEC = "resolve-execution"
SIDECAR_PS_SAMPLERS = "ps-sampler-refs"
SIDECAR_ADMIT = "admit-map"


class Refusal(object):
    """A named refusal. Never a guess, never a default present number."""

    def __init__(self, reason, detail=""):
        self.reason, self.detail = reason, detail

    def __repr__(self):
        return "REFUSE(%s)%s" % (self.reason,
                                 ": " + self.detail if self.detail else "")

    def __eq__(self, o):
        return isinstance(o, Refusal) and o.reason == self.reason


def walk(path, payload_types=(SET_RS,)):
    """Yields (rec, type, dwords7, payload). Payload read only where needed."""
    want = set(payload_types)
    with open(path, "rb") as f:
        magic, ver, recs, pay, presents, _ = struct.unpack("<4sIQQII", f.read(32))
        if magic != b"SRHT":
            sys.exit("not an SRHT trace: " + path)
        for i in range(recs):
            r = f.read(48)
            if len(r) < 48:
                sys.exit("FAIL: truncated walk at record %d of %d" % (i, recs))
            d = struct.unpack("<8I", r[:32])
            pb = struct.unpack("<I", r[32:36])[0]
            if pb and d[0] in want:
                payload = f.read(pb)
            elif pb:
                f.seek(pb, 1)
                payload = None
            else:
                payload = None
            yield i, d[0], d[1:], payload


def decode_state(payload):
    """(colorMask, depthControl) from the guest's render-state block, or None.

    Big-endian dwords; the two register runs are contiguous in MEMORY, not in
    register index -- run 1 (0x2100) is 26 dwords, so 0x2200 lands at dword 26.
    """
    if not payload or len(payload) < 236:
        return None
    n = len(payload) // 4
    if n <= RS_DEPTHCTL_IDX:
        return None
    v = struct.unpack(">%dI" % n, payload[:n * 4])
    return v[RS_COLORMASK_IDX], v[RS_DEPTHCTL_IDX]


def resolve_eligible(rectLT, rectRB, destPoint, destSize, lsf):
    """The guest-side STRUCTURAL prefilter only (hle_video.cpp:1917).

    Passing this does NOT mean the resolve executed -- see correction 2.
    """
    dstW = (destSize & 0x1FFF) + 1
    dstH = ((destSize >> 13) & 0x1FFF) + 1
    full = (rectLT == 0 and rectRB == 0)
    expFull = (rectLT == 0 and destPoint == 0
               and (rectRB & 0xFFFF) == dstW and (rectRB >> 16) == dstH)
    if (not full and not expFull) or destPoint != 0:
        return False
    return not (lsf & 0xF) and not ((lsf >> 4) & 0xFF)


def clear_is_full(t, d):
    """Provably whole-surface? A partial clear is NOT a kill (correction 3)."""
    if t == CLEAR:
        return d[1] == 0 and d[2] == 0      # count == 0, rects == NULL
    return d[2] == 0                        # ClearF: rect == NULL


def route_one(events, target, front_key=None, resolve_exec=None,
              ps_samplers=None, admitted=None):
    """The first present that can show `target`. SINGLETON by construction.

    Returns (present, chain) on success, or (Refusal, chain). Callers must treat
    a Refusal as "do not measure anything" -- never as present 0.
    """
    missing = []
    if resolve_exec is None:
        missing.append(SIDECAR_RESOLVE_EXEC)
    if ps_samplers is None:
        missing.append(SIDECAR_PS_SAMPLERS)
    if admitted is None:
        missing.append(SIDECAR_ADMIT)
    if missing:
        return Refusal("missing-sidecar", ", ".join(missing)), []

    rt0 = ds = 0
    tex, ps = {}, 0
    cmask, dctl, rs_valid = 0xF, 0, False
    t_colour, t_depth, t_key = set(), set(), set()
    armed = False
    present, chain, front = 0, [], front_key

    for rec, t, d, payload in events:
        if t == SET_RT:
            if d[1] == 0:                       # RT0 is the only bound slot
                rt0 = d[2]
        elif t == SET_DS:
            ds = d[1]
        elif t == SET_TEX:
            tex[d[1]] = d[2] & 0x1FFFFFFF
        elif t == SET_SHADERS:
            ps = (d[4] << 32) | d[3]
        elif t == SET_RS:
            st = decode_state(payload)
            if st:
                cmask, dctl = st
                rs_valid = True
        elif t in DRAW_TYPES:
            writes_colour = ((cmask & 0xF) if rs_valid else 0xF) != 0
            writes_depth = bool(ds) and (dctl & Z_WRITE_ENABLE) != 0
            if rec == target:
                armed = True
                if not writes_colour and not writes_depth:
                    return Refusal(
                        "target-writes-nothing",
                        "rec%d has RT0 mask %#x and depth write off, so it "
                        "cannot contribute to any surface" % (rec, cmask & 0xF)
                    ), chain
                if writes_colour and rt0:
                    t_colour.add(rt0)
                    chain.append("rec%d TARGET writes RT0 %#010x (mask %#x)"
                                 % (rec, rt0, cmask & 0xF))
                if writes_depth:
                    t_depth.add(ds)
                    chain.append("rec%d TARGET writes depth %#010x "
                                 "(tracked, not routed -- see below)" % (rec, ds))
            elif armed and writes_colour and rt0:
                # correction 4: an ADMITTED consumer whose PS actually samples
                # the slot carrying the tainted key.
                if rec not in admitted:
                    continue
                used = ps_samplers.get(ps, set())
                hit = [s for s, k in tex.items()
                       if k and k in t_key and s in used]
                if hit:
                    if rt0 not in t_colour:
                        chain.append("rec%d consumer samples tainted key on "
                                     "sampler %s -> taints RT0 %#010x"
                                     % (rec, hit[0], rt0))
                    t_colour.add(rt0)
        elif t in (CLEAR, CLEARF):
            flags = d[3] if t == CLEAR else d[1]
            full = clear_is_full(t, d)
            hits_colour = (flags & CLEAR_COLOR) and rt0 in t_colour
            hits_depth = (flags & CLEAR_DEPTH) and ds in t_depth
            if hits_colour or hits_depth:
                if not full:
                    return Refusal(
                        "ambiguous-partial-clear",
                        "rec%d partially clears a tainted surface; the "
                        "surviving region is not derivable offline" % rec
                    ), chain
                if hits_colour:
                    t_colour.discard(rt0)
                    chain.append("rec%d full colour clear KILLS taint on "
                                 "%#010x" % (rec, rt0))
                if hits_depth:
                    t_depth.discard(ds)
                    chain.append("rec%d full depth clear KILLS depth taint on "
                                 "%#010x" % (rec, ds))
        elif t == RESOLVE:
            src, key = d[0], d[1] & 0x1FFFFFFF
            if not resolve_eligible(d[3], d[4], d[5], d[2], d[6]):
                continue                       # structurally refused: no product
            if not resolve_exec.get(rec, False):
                continue                       # correction 2: never executed
            # correction 3: ASSIGN, so an untainted source overwrites a stale
            # destination rather than leaving it falsely tainted.
            if src in t_colour:
                t_key.add(key)
                chain.append("rec%d resolve %#010x -> key %#010x (tainted)"
                             % (rec, src, key))
            elif key in t_key:
                t_key.discard(key)
                chain.append("rec%d resolve of an UNTAINTED %#010x OVERWRITES "
                             "key %#010x -> taint killed" % (rec, src, key))
        elif t == PRESENT:
            fk = d[3] & 0x1FFFFFFF
            if front is None:
                front = fk
            if armed and fk in t_key:
                chain.append("present %d scans out front key %#010x -> SCANOUT"
                             % (present, fk))
                return present, chain
            present += 1

    if not armed:
        return Refusal("target-not-found",
                       "record %d is not a draw in this trace" % target), chain
    return Refusal("no-reachable-scanout",
                   "the taint never reaches a presented front key"), chain


def route_many(events_factory, targets, **kw):
    """Route each target INDEPENDENTLY, then group equal sinks (correction 5).

    Returns (present, chain) only when every target agrees. Differing sinks are
    a REFUSAL carrying the split, never an average or a first-wins answer.
    """
    per = {}
    for tgt in sorted(targets):
        p, ch = route_one(events_factory(), tgt, **kw)
        per[tgt] = (p, ch)
    sinks = {}
    for tgt, (p, _ch) in per.items():
        if isinstance(p, Refusal):
            return Refusal("member-refused",
                           "record %d: %r" % (tgt, p)), per
        sinks.setdefault(p, []).append(tgt)
    if len(sinks) > 1:
        return Refusal(
            "differing-sinks",
            "; ".join("present %d <- %s" % (p, t) for p, t in
                      sorted(sinks.items()))), per
    return list(sinks)[0], per


# ─────────────────────────────────────────────────────────────────────────────


def self_test():
    """Synthetic routes. No trace, no replay, no sidecars from disk."""
    npass = nfail = 0

    def ck(ok, what):
        nonlocal npass, nfail
        if ok:
            npass += 1
            print("  PASS  " + what)
        else:
            nfail += 1
            print("  FAIL  " + what)

    FRONT = 0x0A94E000

    def ev(rec, t, *d, **kw):
        return (rec, t, tuple(list(d) + [0] * (7 - len(d))), kw.get("payload"))

    def state(mask, depth=0):
        """A synthetic 236-byte render-state block."""
        v = [0] * 59
        v[RS_COLORMASK_IDX] = mask
        v[RS_DEPTHCTL_IDX] = depth
        return struct.pack(">59I", *v)

    def rs(rec, mask, depth=0):
        return ev(rec, SET_RS, payload=state(mask, depth))

    # sidecars: every resolve executed, PS 0 samples every slot, all admitted
    ALLEXEC = _AllTrue()
    ALLSAMP = _AllSlots()
    ALLADM = _AllIn()

    print("SCANOUT ROUTE r2 (synthetic; no trace, no replay)")
    print("\n-- baseline route --")

    base = [ev(0, SET_RT, 0, 0, 0xAAAA), rs(1, 0xF), ev(2, 10),
            ev(3, RESOLVE, 0xAAAA, FRONT, 0, 0, 0, 0, 0),
            ev(4, PRESENT, 0, 0, 0, FRONT)]
    p, ch = route_one(base, 2, resolve_exec=ALLEXEC, ps_samplers=ALLSAMP,
                      admitted=ALLADM)
    ck(p == 0, "a draw resolved straight to the front key scans out there "
               "(got %r)" % p)

    later = [ev(0, SET_RT, 0, 0, 0xAAAA), rs(1, 0xF), ev(2, 10),
             ev(3, PRESENT, 0, 0, 0, 0x1111),
             ev(4, RESOLVE, 0xAAAA, FRONT, 0, 0, 0, 0, 0),
             ev(5, PRESENT, 0, 0, 0, FRONT)]
    p, ch = route_one(later, 2, resolve_exec=ALLEXEC, ps_samplers=ALLSAMP,
                      admitted=ALLADM)
    ck(p == 1, "scanout is the first present SHOWING the product, not the "
               "draw's execution present (got %r)" % p)

    # ── correction 1: RT0 only, gated on the write mask ──────────────────────
    print("\n-- correction 1: RT0 only, gated on the write mask --")

    # KILLER: RT1-only false propagation. r1 tainted every bound MRT, so a
    # resolve of RT1 carried the draw. The executor never binds RT1.
    rt1 = [ev(0, SET_RT, 0, 0, 0xAAAA), ev(1, SET_RT, 0, 1, 0xBBBB),
           rs(2, 0xF), ev(3, 10),
           ev(4, RESOLVE, 0xBBBB, FRONT, 0, 0, 0, 0, 0),
           ev(5, PRESENT, 0, 0, 0, FRONT)]
    p, ch = route_one(rt1, 3, resolve_exec=ALLEXEC, ps_samplers=ALLSAMP,
                      admitted=ALLADM)
    ck(isinstance(p, Refusal) and p.reason == "no-reachable-scanout",
       "KILLER a resolve of RT1 does NOT carry the draw -- the executor binds "
       "NumRenderTargets=1 (got %r)" % p)

    masked = [ev(0, SET_RT, 0, 0, 0xAAAA), rs(1, 0x0), ev(2, 10),
              ev(3, RESOLVE, 0xAAAA, FRONT, 0, 0, 0, 0, 0),
              ev(4, PRESENT, 0, 0, 0, FRONT)]
    p, ch = route_one(masked, 2, resolve_exec=ALLEXEC, ps_samplers=ALLSAMP,
                      admitted=ALLADM)
    ck(isinstance(p, Refusal) and p.reason == "target-writes-nothing",
       "a draw with a ZERO RT0 write mask and no depth write is refused, not "
       "routed (got %r)" % p)

    nostate = [ev(0, SET_RT, 0, 0, 0xAAAA), ev(1, 10),
               ev(2, RESOLVE, 0xAAAA, FRONT, 0, 0, 0, 0, 0),
               ev(3, PRESENT, 0, 0, 0, FRONT)]
    p, ch = route_one(nostate, 1, resolve_exec=ALLEXEC, ps_samplers=ALLSAMP,
                      admitted=ALLADM)
    ck(p == 0, "with NO render state the mask defaults to 0xF, mirroring "
               "d3d12_backend.cpp:4258 (got %r)" % p)

    # depth-only writer: tracked, but depth is NOT a routing path
    donly = [ev(0, SET_RT, 0, 0, 0xAAAA), ev(1, SET_DS, 0, 0xDDDD),
             rs(2, 0x0, Z_WRITE_ENABLE), ev(3, 10),
             ev(4, RESOLVE, 0xAAAA, FRONT, 0, 0, 0, 0, 0),
             ev(5, PRESENT, 0, 0, 0, FRONT)]
    p, ch = route_one(donly, 3, resolve_exec=ALLEXEC, ps_samplers=ALLSAMP,
                      admitted=ALLADM)
    ck(isinstance(p, Refusal) and p.reason == "no-reachable-scanout",
       "a depth-only writer is TRACKED but never routed through colour "
       "(got %r)" % p)
    ck(any("depth" in s for s in ch),
       "...and the depth write is still reported in the chain")

    # ── correction 2: executed, not merely eligible ──────────────────────────
    print("\n-- correction 2: EXECUTED, not merely eligible --")

    # KILLER: structurally eligible but backend-refused.
    p, ch = route_one(base, 2, resolve_exec={}, ps_samplers=ALLSAMP,
                      admitted=ALLADM)
    ck(isinstance(p, Refusal) and p.reason == "no-reachable-scanout",
       "KILLER a STRUCTURALLY ELIGIBLE resolve the backend REFUSED carries no "
       "taint (got %r)" % p)

    sliced = [ev(0, SET_RT, 0, 0, 0xAAAA), rs(1, 0xF), ev(2, 10),
              ev(3, RESOLVE, 0xAAAA, FRONT, 0, 0, 0, 0, 1),
              ev(4, PRESENT, 0, 0, 0, FRONT)]
    p, ch = route_one(sliced, 2, resolve_exec=ALLEXEC, ps_samplers=ALLSAMP,
                      admitted=ALLADM)
    ck(isinstance(p, Refusal),
       "a structurally refused (sliced) resolve still carries nothing")

    p, ch = route_one(base, 2, ps_samplers=ALLSAMP, admitted=ALLADM)
    ck(isinstance(p, Refusal) and p.reason == "missing-sidecar"
       and SIDECAR_RESOLVE_EXEC in p.detail,
       "with NO execution sidecar the tool refuses BY NAME rather than "
       "assuming success (got %r)" % p)

    # ── correction 3: kill semantics ─────────────────────────────────────────
    print("\n-- correction 3: kill semantics --")

    # KILLER: a full clear must remove the draw's contribution.
    cleared = [ev(0, SET_RT, 0, 0, 0xAAAA), rs(1, 0xF), ev(2, 10),
               ev(3, CLEAR, 0, 0, 0, CLEAR_COLOR),
               ev(4, RESOLVE, 0xAAAA, FRONT, 0, 0, 0, 0, 0),
               ev(5, PRESENT, 0, 0, 0, FRONT)]
    p, ch = route_one(cleared, 2, resolve_exec=ALLEXEC, ps_samplers=ALLSAMP,
                      admitted=ALLADM)
    ck(isinstance(p, Refusal) and p.reason == "no-reachable-scanout",
       "KILLER a FULL COLOUR CLEAR after the draw kills its taint (got %r)" % p)

    partial = [ev(0, SET_RT, 0, 0, 0xAAAA), rs(1, 0xF), ev(2, 10),
               ev(3, CLEAR, 0, 1, 0x8000, CLEAR_COLOR),
               ev(4, RESOLVE, 0xAAAA, FRONT, 0, 0, 0, 0, 0),
               ev(5, PRESENT, 0, 0, 0, FRONT)]
    p, ch = route_one(partial, 2, resolve_exec=ALLEXEC, ps_samplers=ALLSAMP,
                      admitted=ALLADM)
    ck(isinstance(p, Refusal) and p.reason == "ambiguous-partial-clear",
       "a PARTIAL clear over a tainted surface is AMBIGUOUS -- refuse, do not "
       "guess either way (got %r)" % p)

    depthclr = [ev(0, SET_RT, 0, 0, 0xAAAA), rs(1, 0xF), ev(2, 10),
                ev(3, CLEAR, 0, 0, 0, CLEAR_DEPTH),
                ev(4, RESOLVE, 0xAAAA, FRONT, 0, 0, 0, 0, 0),
                ev(5, PRESENT, 0, 0, 0, FRONT)]
    p, ch = route_one(depthclr, 2, resolve_exec=ALLEXEC, ps_samplers=ALLSAMP,
                      admitted=ALLADM)
    ck(p == 0, "a DEPTH clear (0x10) does not kill COLOUR taint -- 360 flag "
               "semantics, not PC D3D9 (got %r)" % p)

    # KILLER: destination overwrite. An untainted source resolved over our
    # product must ASSIGN, not OR, or a dead product nominates a later scanout.
    overwrite = [ev(0, SET_RT, 0, 0, 0xAAAA), rs(1, 0xF), ev(2, 10),
                 ev(3, RESOLVE, 0xAAAA, 0x0BBBB000, 0, 0, 0, 0, 0),
                 ev(4, SET_RT, 0, 0, 0xEEEE), rs(5, 0xF), ev(6, 10),
                 ev(7, RESOLVE, 0xEEEE, 0x0BBBB000, 0, 0, 0, 0, 0),
                 ev(8, SET_RT, 0, 0, 0xCCCC), rs(9, 0xF),
                 ev(10, SET_TEX, 0, 5, 0x0BBBB000), ev(11, 11),
                 ev(12, RESOLVE, 0xCCCC, FRONT, 0, 0, 0, 0, 0),
                 ev(13, PRESENT, 0, 0, 0, FRONT)]
    p, ch = route_one(overwrite, 2, resolve_exec=ALLEXEC, ps_samplers=ALLSAMP,
                      admitted=ALLADM)
    ck(isinstance(p, Refusal) and p.reason == "no-reachable-scanout",
       "KILLER an UNTAINTED resolve over our product OVERWRITES it -- the dead "
       "product cannot nominate a scanout (got %r)" % p)
    ck(any("OVERWRITES" in s for s in ch),
       "...and the overwrite is named in the chain, not silent")

    # ── correction 4: bound is not sampled ───────────────────────────────────
    print("\n-- correction 4: a BOUND texture is not a SAMPLED one --")

    chain_ev = [ev(0, SET_RT, 0, 0, 0xAAAA), rs(1, 0xF), ev(2, 10),
                ev(3, RESOLVE, 0xAAAA, 0x0BBBB000, 0, 0, 0, 0, 0),
                ev(4, SET_RT, 0, 0, 0xCCCC), rs(5, 0xF),
                ev(6, SET_SHADERS, 0, 0, 0, 0x77, 0),
                ev(7, SET_TEX, 0, 5, 0x0BBBB000), ev(8, 11),
                ev(9, RESOLVE, 0xCCCC, FRONT, 0, 0, 0, 0, 0),
                ev(10, PRESENT, 0, 0, 0, FRONT)]
    p, ch = route_one(chain_ev, 2, resolve_exec=ALLEXEC,
                      ps_samplers={0x77: {5}}, admitted=ALLADM)
    ck(p == 0, "taint propagates when the PS genuinely samples that slot "
               "(got %r)" % p)

    # KILLER: the consumer binds the key on slot 5 but its PS reads only slot 2.
    p, ch = route_one(chain_ev, 2, resolve_exec=ALLEXEC,
                      ps_samplers={0x77: {2}}, admitted=ALLADM)
    ck(isinstance(p, Refusal) and p.reason == "no-reachable-scanout",
       "KILLER a consumer that BINDS the product on a slot its PS never reads "
       "does NOT propagate (got %r)" % p)

    # KILLER: the consumer samples it, but the consumer was itself rejected.
    p, ch = route_one(chain_ev, 2, resolve_exec=ALLEXEC,
                      ps_samplers={0x77: {5}}, admitted={2})
    ck(isinstance(p, Refusal) and p.reason == "no-reachable-scanout",
       "KILLER a REJECTED consumer does not propagate even when it samples the "
       "product (got %r)" % p)

    p, ch = route_one(chain_ev, 2, resolve_exec=ALLEXEC, admitted=ALLADM)
    ck(isinstance(p, Refusal) and SIDECAR_PS_SAMPLERS in p.detail,
       "with NO sampler sidecar the tool refuses BY NAME (got %r)" % p)

    # ── correction 5: singleton routing ──────────────────────────────────────
    print("\n-- correction 5: one draw per call, sinks grouped or split --")

    # KILLER: a record that is not a draw at all.
    p, ch = route_one(base, 999, resolve_exec=ALLEXEC, ps_samplers=ALLSAMP,
                      admitted=ALLADM)
    ck(isinstance(p, Refusal) and p.reason == "target-not-found",
       "KILLER a MISSING target record is refused by name, not silently "
       "unrouted (got %r)" % p)

    # two draws, DIFFERENT sinks: r1 would have returned the first and stopped.
    two = [ev(0, SET_RT, 0, 0, 0xAAAA), rs(1, 0xF), ev(2, 10),
           ev(3, RESOLVE, 0xAAAA, FRONT, 0, 0, 0, 0, 0),
           ev(4, PRESENT, 0, 0, 0, FRONT),
           ev(5, SET_RT, 0, 0, 0xBBBB), rs(6, 0xF), ev(7, 10),
           ev(8, RESOLVE, 0xBBBB, FRONT, 0, 0, 0, 0, 0),
           ev(9, PRESENT, 0, 0, 0, FRONT)]
    p, per = route_many(lambda: list(two), {2, 7}, resolve_exec=ALLEXEC,
                        ps_samplers=ALLSAMP, admitted=ALLADM)
    ck(isinstance(p, Refusal) and p.reason == "differing-sinks",
       "KILLER two draws with DIFFERENT sinks REFUSE and report the split "
       "rather than returning the first (got %r)" % p)
    ck(per[2][0] == 0 and per[7][0] == 1,
       "...and each member's own sink is reported (%r, %r)"
       % (per[2][0], per[7][0]))

    same = [ev(0, SET_RT, 0, 0, 0xAAAA), rs(1, 0xF), ev(2, 10), ev(3, 10),
            ev(4, RESOLVE, 0xAAAA, FRONT, 0, 0, 0, 0, 0),
            ev(5, PRESENT, 0, 0, 0, FRONT)]
    p, per = route_many(lambda: list(same), {2, 3}, resolve_exec=ALLEXEC,
                        ps_samplers=ALLSAMP, admitted=ALLADM)
    ck(p == 0, "two draws with the SAME sink group into one present (got %r)"
       % p)

    p, per = route_many(lambda: list(two), {2, 999}, resolve_exec=ALLEXEC,
                        ps_samplers=ALLSAMP, admitted=ALLADM)
    ck(isinstance(p, Refusal) and p.reason == "member-refused",
       "one refusing member refuses the whole group (got %r)" % p)

    # ── the r1 rules would have passed some of these; prove r1 is dead ───────
    print("\n-- the retired r1 assumption --")
    ck(not hasattr(sys.modules[__name__], "route"),
       "the r1 `route()` entry point is GONE, not left beside its replacement")

    total = npass + nfail
    print("\n%d/%d route fixtures behave as specified" % (npass, total))
    if nfail:
        print("SELF-TEST FAILED")
    return 0 if nfail == 0 else 1


class _AllTrue(dict):
    def get(self, k, d=None):
        return True


class _AllSlots(dict):
    def get(self, k, d=None):
        return set(range(16))


class _AllIn(object):
    def __contains__(self, k):
        return True


def load_resolve_exec(path):
    """`<record> <0|1> [outcome]` per line: did that resolve EXECUTE?

    None (no path) means the sidecar does not exist, which the router must
    treat as REFUSE -- never as "assume it executed".

    ⚠ THE SINGLE DEFINITION. `sweep_driver` imports this rather than
    keeping its own copy: the director found the CLI and the driver had
    DRIFTED, with the driver loading sidecars while the router's own
    `main()` hardcoded both to None and silently ignored its flags.
    """
    if not path:
        return None
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.split("#", 1)[0].split()
        if len(p) >= 2:
            out[int(p[0])] = p[1] not in ("0", "false", "no")
    return out


def load_ps_samplers(path):
    """`<pshash-hex> <slot,slot,...>` per line: which samplers the PS reads.

    Absent and EMPTY are different answers: absent (None) means unknown and
    the router refuses; a shader present with no slots genuinely samples
    nothing.
    """
    if not path:
        return None
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.split("#", 1)[0].split()
        if len(p) >= 2:
            out[int(p[0], 16)] = {int(x) for x in p[1].split(",") if x}
        elif len(p) == 1:
            out[int(p[0], 16)] = set()
    return out


def load_admit(path):
    """record -> outcome, from the SR_DRAW_ADMIT_MAP dump."""
    adm = set()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 3 and parts[-1] == "admitted":
                try:
                    adm.add(int(parts[0]))
                except ValueError:
                    pass
    return adm


def _print_doc():
    """Print the module docstring even on a cp1252 console.

    The usage path CRASHED with UnicodeEncodeError on the arrows and
    warning glyphs in these docstrings, so a user who forgot a flag got a
    traceback and exit 1 instead of usage and the documented exit code.
    The director's acceptance commands set PYTHONUTF8=1, which hid it.
    """
    try:
        print(__doc__)
    except UnicodeEncodeError:
        enc = sys.stdout.encoding or "ascii"
        print(__doc__.encode(enc, "replace").decode(enc))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace", nargs="?")
    ap.add_argument("--record", type=int, action="append", default=[])
    ap.add_argument("--admit")
    ap.add_argument("--resolve-exec", help=SIDECAR_RESOLVE_EXEC + " sidecar")
    ap.add_argument("--ps-samplers", help=SIDECAR_PS_SAMPLERS + " sidecar")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not a.trace or not a.record:
        _print_doc()
        return 2

    # ⛔ THE DEFECT THE DIRECTOR CAUGHT. These two lines read
    #     rexec = None   # no producer exists yet
    #     psamp = None
    # which was true when written -- and then BOTH producers were built in
    # the same session and this was never revisited. The flags existed in
    # argparse and did nothing, so supplying them still refused. An
    # advertised option that is silently discarded is worse than no option.
    admitted = load_admit(a.admit) if a.admit else None
    rexec = load_resolve_exec(a.resolve_exec)
    psamp = load_ps_samplers(a.ps_samplers)

    if len(a.record) == 1:
        p, chain = route_one(walk(a.trace), a.record[0], admitted=admitted,
                             resolve_exec=rexec, ps_samplers=psamp)
    else:
        p, chain = route_many(lambda: walk(a.trace), set(a.record),
                              admitted=admitted, resolve_exec=rexec,
                              ps_samplers=psamp)
        chain = []

    print("records: %s" % a.record)
    for step in chain:
        print("   " + step)
    if isinstance(p, Refusal):
        print("\nREFUSED: %s" % p.detail or p.reason)
        if p.reason == "missing-sidecar":
            print("\nThis is the DESIGNED outcome, not a bug. Two of the "
                  "director's five rules need data the trace does not carry;\n"
                  "see SIDECARS in this file's docstring for what each would "
                  "cost to produce.")
        return 1
    print("\nfirst causally reachable scanout: present %d" % p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
