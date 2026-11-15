"""gen_ps_sampler_map.py - the `ps-sampler-refs` sidecar, derived OFFLINE.

The scanout router's rule 4 (director): a BOUND texture is not a SAMPLED one.
The backend publishes all sixteen sampler slots to the descriptor table on every
draw (d3d12_backend.cpp:4342), so a bind proves nothing about a read. The router
therefore needs, per pixel shader, the set of slots its DXIL actually samples --
and refuses without it.

    python tools/gen_ps_sampler_map.py --self-test
    python tools/gen_ps_sampler_map.py -o archive/iter457_rex/ps_samplers.txt

I first concluded this needed a change to the shader-translation pipeline. It
does not. The shaders are on disk as signed DXIL keyed by the SAME hash the
trace records (`data/shader_cache/dxil/<pshash>.ps.dxil`, ShaderCache::findPs),
and `dxc -dumpbin` disassembles them.

── how a slot is recovered ──────────────────────────────────────────────────

These are BINDLESS shaders: PSV0 reports SRV and Sampler as unbounded ranges,
so the container's resource table says nothing. The index is a constant, and the
constant buffer IS the slot table:

    shared[s]        = 2D texture descriptor for sampler s   (linear  0..15)
    shared[16 + s]   = 3D texture descriptor for sampler s   (linear 16..31)
    shared[48 + s]   = sampler descriptor for sampler s      (linear 48..63)
                                            (d3d12_backend.cpp:4342/4385/4415)

`cbufferLoadLegacy(handle, ROW)` + `extractvalue COMP` is linear index
4*ROW + COMP; an `add i32 %v, K` before the handle adds K. So:

    %6  = cbufferLoadLegacy(..., i32 0)     ; row 0
    %7  = extractvalue %6, 1                ; comp 1  -> linear 1
    %8  = add i32 %7, 0                     ; + 0
    %9  = createHandle(i32 57, i8 0, ...)   ; resourceClass 0 = SRV
                                            => samples slot 1

⚠ SR_S4_YYY=1 aliases `shared[1] = shared[2] = shared[0]`, which would make a
shader reading index 1 sample slot 0's texture. It is a DIAGNOSTIC, default OFF
and never baked, so this map describes the default build only -- and says so, in
the file it writes, rather than leaving a reader to find out.

Read-only: reads the DXIL cache and writes one text file.
"""
import argparse, os, re, subprocess, sys, glob

DXC_CANDIDATES = [
    r"C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/dxc.exe",
    r"C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64/dxc.exe",
    "dxc",
]
CACHE = "data/shader_cache/dxil"

# resourceClass in dx.op.createHandle: 0 = SRV, 1 = UAV, 2 = CBV, 3 = Sampler
RC_SRV, RC_SAMPLER = 0, 3

RE_CBLOAD = re.compile(
    r"^\s*(%\w+) = call %dx\.types\.CBufRet\.i32 "
    r"@dx\.op\.cbufferLoadLegacy\.i32\(i32 \d+, %dx\.types\.Handle %\w+, "
    r"i32 (\d+)\)")
RE_EXTRACT = re.compile(
    r"^\s*(%\w+) = extractvalue %dx\.types\.CBufRet\.i32 (%\w+), (\d+)")
RE_ADD = re.compile(r"^\s*(%\w+) = add i32 (%\w+), (-?\d+)")
RE_HANDLE = re.compile(
    r"^\s*(%\w+) = call %dx\.types\.Handle @dx\.op\.createHandle\("
    r"i32 \d+, i8 (\d+), i32 \d+, i32 (%\w+|\d+),")
# A texture is SAMPLED by a sample/load instruction, not by having a handle
# made for it. dx.op.getDimensions also takes an SRV handle but reads only
# metadata -- counting it would propagate taint through a shader that never
# reads a texel. This is what rule 4 actually asks for: a sampler the pixel
# shader REFERENCES.
RE_SAMPLE = re.compile(
    r"@dx\.op\.(sample[A-Za-z]*|textureLoad|textureGather[A-Za-z]*)\.\w+\("
    r"i32 \d+, %dx\.types\.Handle (%\w+)"
    # sample ops carry the sampler as the second handle; textureLoad has none.
    r"(?:, %dx\.types\.Handle (%\w+))?")


def find_dxc():
    for c in DXC_CANDIDATES:
        if os.path.sep in c or "/" in c:
            if os.path.isfile(c):
                return c
        else:
            try:
                subprocess.run([c, "--version"], capture_output=True, timeout=20)
                return c
            except Exception:
                pass
    return None


def slots_from_ir(ir):
    """(srv_slots, sampler_slots, unresolved) for one disassembled shader.

    `unresolved` counts SRV handles whose index could NOT be traced back to a
    constant-buffer load. It is reported, never silently dropped: a shader with
    an unresolved index is one whose sampled set we do not know, and the caller
    must treat that as unknown rather than empty.
    """
    cbrow, comp, addv = {}, {}, {}
    handles = {}            # ssa name -> (resourceClass, linear index)
    sampled = []            # ssa names appearing as a sample/load SRV
    srv, samp, cube = set(), set(), set()
    unresolved = 0

    def linear(v):
        """Resolve an SSA value to a linear constant-buffer index, or None."""
        k = 0
        seen = 0
        while v in addv and seen < 16:
            v, d = addv[v]
            k += d
            seen += 1
        if v not in comp:
            return None
        src, c = comp[v]
        if src not in cbrow:
            return None
        return 4 * cbrow[src] + c + k

    for line in ir.splitlines():
        m = RE_CBLOAD.match(line)
        if m:
            cbrow[m.group(1)] = int(m.group(2))
            continue
        m = RE_EXTRACT.match(line)
        if m:
            comp[m.group(1)] = (m.group(2), int(m.group(3)))
            continue
        m = RE_ADD.match(line)
        if m:
            addv[m.group(1)] = (m.group(2), int(m.group(3)))
            continue
        m = RE_HANDLE.match(line)
        if m:
            rc, idx = int(m.group(2)), m.group(3)
            if rc not in (RC_SRV, RC_SAMPLER):
                continue
            lin = None if idx.startswith("%") else int(idx)
            if lin is None:
                lin = linear(idx)
            handles[m.group(1)] = (rc, lin)
            continue
        for m in RE_SAMPLE.finditer(line):
            sampled.append((m.group(2), m.group(3)))

    # ── only handles that an instruction actually READS ─────────────────
    for hname, sname in sampled:
        if sname is not None:
            src, slin = handles.get(sname, (None, None))
            if src == RC_SAMPLER and slin is not None and 48 <= slin < 64:
                samp.add(slin - 48)
        rc, lin = handles.get(hname, (None, None))
        if rc != RC_SRV:
            continue
        if lin is None:
            # "we do not know" is NOT "samples nothing" -- the caller must
            # treat an unresolved shader as unknown and refuse on it.
            unresolved += 1
            continue
        # 0..15 = 2D slot s; 16..31 = 3D slot s-16. Both are sampler slot s
        # -- the compile-time tfetch dimension picks the range
        # (d3d12_backend.cpp:4385).
        if lin < 16:
            srv.add(lin)
        elif lin < 32:
            srv.add(lin - 16)
        elif lin < 48:
            # ⚠ CUBE, and the backend NEVER WRITES shared[32..47] (it fills
            # 2D 0..15, 3D 16..31 and samplers 48..63 only). So this fetch
            # reads descriptor index 0 out of the cube heap, and it CANNOT
            # read a resolved render product, which is registered in the 2D
            # heap via shared[s] = d.texSlot[s]. Excluded from the sampled
            # set deliberately -- and COUNTED, because silently dropping a
            # range is how a false negative gets built.
            cube.add(lin - 32)
    return srv, samp, unresolved, cube


def slot_bands_from_ir(ir):
    """slot -> the set of BANDS this shader samples it through.

    `slots_from_ir` folds 2D/3D/cube onto the sampler slot, which is what
    the router needs. The band conformance check needs the fold UNDONE:
    "is this product read as 2D or as 3D" is exactly the question, and a
    folded slot cannot answer it.

        linear 0..15 = 2D, 16..31 = 3D, 32..47 = CUBE  (ENGINE_MAP 6g.3c)
    """
    cbrow, comp, addv, handles, sampled = {}, {}, {}, {}, []
    for line in ir.splitlines():
        m = RE_CBLOAD.match(line)
        if m:
            cbrow[m.group(1)] = int(m.group(2)); continue
        m = RE_EXTRACT.match(line)
        if m:
            comp[m.group(1)] = (m.group(2), int(m.group(3))); continue
        m = RE_ADD.match(line)
        if m:
            addv[m.group(1)] = (m.group(2), int(m.group(3))); continue
        m = RE_HANDLE.match(line)
        if m:
            rc, idx = int(m.group(2)), m.group(3)
            lin = None if idx.startswith("%") else int(idx)
            if lin is None:
                v, k = idx, 0
                seen = 0
                while v in addv and seen < 16:
                    v, dd = addv[v]; k += dd; seen += 1
                if v in comp:
                    src, c = comp[v]
                    if src in cbrow:
                        lin = 4 * cbrow[src] + c + k
            handles[m.group(1)] = (rc, lin); continue
        for m in RE_SAMPLE.finditer(line):
            sampled.append(m.group(2))
    out = {}
    for hn in sampled:
        rc, lin = handles.get(hn, (None, None))
        if rc != RC_SRV or lin is None or lin >= 48:
            continue
        band = "2D" if lin < 16 else ("3D" if lin < 32 else "CUBE")
        out.setdefault(lin % 16, set()).add(band)
    return out


def self_test():
    npass = nfail = 0

    def ck(ok, what):
        nonlocal npass, nfail
        if ok:
            npass += 1
            print("  PASS  " + what)
        else:
            nfail += 1
            print("  FAIL  " + what)

    print("PS SAMPLER MAP (synthetic IR; no dxc, no shaders)")

    # the real shape, lifted verbatim from 059eeadd2781ba31
    ir = """
  %6 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %1, i32 0)
  %7 = extractvalue %dx.types.CBufRet.i32 %6, 1
  %8 = add i32 %7, 0
  %9 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 0, i32 %8, i1 false)
  %4 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %1, i32 12)
  %5 = extractvalue %dx.types.CBufRet.i32 %4, 1
  %19 = add i32 %5, 0
  %20 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 3, i32 0, i32 %19, i1 false)
  %10 = call %dx.types.Dimensions @dx.op.getDimensions(i32 72, %dx.types.Handle %9, i32 0)
  %21 = call %dx.types.ResRet.f32 @dx.op.sample.f32(i32 60, %dx.types.Handle %9, %dx.types.Handle %20, float %17, float %18, float undef, float undef, i32 0, i32 0, i32 undef, float undef)
"""
    srv, samp, unres, _cb = slots_from_ir(ir)
    ck(srv == {1}, "row 0 component 1 resolves to texture slot 1 (got %s)" % srv)
    ck(samp == {1}, "row 12 component 1 resolves to sampler slot 1 (got %s)"
       % samp)
    ck(unres == 0, "and nothing is left unresolved")

    # KILLER: slot 0 and slot 1 must not be confused - the whole rec9587
    # question turned on exactly this one component index.
    ir0 = ir.replace("%6, 1", "%6, 0")
    srv0, _s, _u, _c = slots_from_ir(ir0)
    ck(srv0 == {0},
       "KILLER component 0 resolves to slot 0, NOT slot 1 (got %s)" % srv0)

    # a 3D fetch reads the 16..31 range and maps back to the same sampler slot
    ir3 = ir.replace("i32 0)\n  %7", "i32 4)\n  %7")
    srv3, _s, _u, _c = slots_from_ir(ir3)
    ck(srv3 == {1}, "a 3D fetch at linear 17 maps back to sampler slot 1 "
                    "(got %s)" % srv3)

    # KILLER: an index we cannot trace must be UNRESOLVED, never empty --
    # "we don't know" and "samples nothing" are opposite answers for the router.
    irx = """
  %9 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 0, i32 %99, i1 false)
  %21 = call %dx.types.ResRet.f32 @dx.op.sample.f32(i32 60, %dx.types.Handle %9, %dx.types.Handle %20, float %17, float %18, float undef, float undef, i32 0, i32 0, i32 undef, float undef)
"""
    srvx, _s, unresx, _c = slots_from_ir(irx)
    ck(srvx == set() and unresx == 1,
       "KILLER an untraceable SRV index counts as UNRESOLVED, not as an empty "
       "sampled set (got %s / %d)" % (srvx, unresx))

    # the add offset participates
    ira = ir.replace("add i32 %7, 0", "add i32 %7, 2")
    srva, _s, _u, _c = slots_from_ir(ira)
    ck(srva == {3}, "an `add i32 %%v, 2` shifts the linear index (got %s)"
       % srva)

    # a CBV handle is not a texture read
    irc = ir.replace("i8 0, i32 0, i32 %8", "i8 2, i32 0, i32 %8")
    srvc, _s, _u, _c = slots_from_ir(irc)
    ck(srvc == set(), "a CBV handle is not counted as a sampled texture")

    # KILLER: GetDimensions reads METADATA, not texels. Counting it would
    # propagate taint through a shader that never reads a single texel of the
    # product - and the real shaders do call it, on the very handle in question.
    ird = "\n".join(l for l in ir.splitlines() if "dx.op.sample" not in l)
    srvd, _s, _u, _c = slots_from_ir(ird)
    ck(srvd == set(),
       "KILLER a texture referenced ONLY by GetDimensions is NOT sampled "
       "(got %s)" % srvd)

    total = npass + nfail
    print("\n%d/%d sampler-map fixtures behave as specified" % (npass, total))
    return 0 if nfail == 0 else 1


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
    ap.add_argument("-o", "--out")
    ap.add_argument("--bands-out",
                    help="also write pshash -> slot:BAND,... for "
                         "tools/check_band_conformance.py")
    ap.add_argument("--cache", default=CACHE)
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not a.out:
        _print_doc()
        return 2

    dxc = find_dxc()
    if not dxc:
        print("FAIL: dxc.exe not found; cannot disassemble. Tried:")
        for c in DXC_CANDIDATES:
            print("   " + c)
        return 1

    files = sorted(glob.glob(os.path.join(a.cache, "*.ps.dxil")))
    if not files:
        print("FAIL: no *.ps.dxil under " + a.cache)
        return 1
    print("dxc: %s" % dxc)
    print("shaders: %d" % len(files))

    rows, failed, unres_total, unres_shaders = [], 0, 0, []
    bandrows = []
    for i, f in enumerate(files):
        h = os.path.basename(f).split(".")[0]
        r = subprocess.run([dxc, "-dumpbin", f], capture_output=True,
                           text=True, errors="replace")
        if r.returncode != 0:
            failed += 1
            continue
        srv, samp, unres, cb = slots_from_ir(r.stdout)
        bandrows.append((h, slot_bands_from_ir(r.stdout)))
        unres_total += unres
        if unres:
            unres_shaders.append(h)
        rows.append((h, sorted(srv), sorted(samp), unres, sorted(cb)))
        if (i + 1) % 100 == 0:
            print("  ... %d/%d" % (i + 1, len(files)))

    with open(a.out, "w", newline="\n") as fh:
        fh.write("# ps-sampler-refs: <pshash-hex> <slot,slot,...>\n")
        fh.write("# generated by tools/gen_ps_sampler_map.py from %s\n" % a.cache)
        fh.write("# DEFAULT BUILD ONLY: SR_S4_YYY=1 aliases shared[1]=shared[2]"
                 "=shared[0] and would invalidate slots 1 and 2 here.\n")
        fh.write("# a shader with UNRESOLVED indices is omitted, so the router "
                 "refuses on it rather than reading it as sampling nothing.\n")
        for h, srv, samp, unres, cb in rows:
            if unres:
                continue
            fh.write("%s %s\n" % (h, ",".join(str(s) for s in srv)))

    if a.bands_out:
        with open(a.bands_out, "w", newline="\n") as fh:
            fh.write("# pshash-hex  slot:BAND,slot:BAND,...  (BAND = 2D|3D|CUBE)\n")
            fh.write("# generated by tools/gen_ps_sampler_map.py --bands-out\n")
            for h, m in bandrows:
                toks = ["%d:%s" % (sl, b)
                        for sl in sorted(m) for b in sorted(m[sl])]
                # `h` is the hex FILENAME stem, already the pshash - not an int.
                fh.write("%s %s\n" % (h, ",".join(toks) or "-"))
        print("wrote %s (%d shaders)" % (a.bands_out, len(bandrows)))

    emitted = sum(1 for r in rows if not r[3])
    print("\nwrote %s" % a.out)
    print("  shaders disassembled : %d" % len(rows))
    print("  emitted              : %d" % emitted)
    print("  omitted (unresolved) : %d  (%d unresolved SRV handles)"
          % (len(rows) - emitted, unres_total))
    print("  dxc failures         : %d" % failed)
    # CONSERVATION: every input file is accounted for in exactly one bucket.
    tot = emitted + (len(rows) - emitted) + failed
    print("  accounted for        : %d of %d %s"
          % (tot, len(files), "OK" if tot == len(files) else "MISMATCH"))
    # Cross-check: the sampler-descriptor slots should agree with the SRV slots.
    dis = [h for h, srv, samp, u, cb in rows
           if not u and samp and set(srv) | set(cb) != set(samp)]
    ncube = [h for h, srv, samp, u, cb in rows if cb]
    print("  srv+cube vs sampler disagreement: %d shader(s)%s"
          % (len(dis), (" e.g. " + dis[0]) if dis else ""))
    print("  shaders issuing CUBE fetches   : %d of %d  <- shared[32..47] is "
          "NEVER written by the backend" % (len(ncube), len(rows)))
    return 0 if tot == len(files) else 1


if __name__ == "__main__":
    sys.exit(main())
