#!/usr/bin/env python3
"""
audit_forcing.py -- structural forcing / band-aid checker (charter enforcement).

Turns "are there hidden band-aids?" into a COMMAND you run, not a claim you trust.

It finds EVERY guest-state-writing / guest-driving primitive in the runtime
(kernel/, gfx/, app/) -- the surface through which forcing happens -- and reports,
for each call site, whether it is:

  MASTER   gated behind forcingAllowed()/SR_ALLOW_FORCING (the one true gate; OK)
  OWNGATE  behind a private SR_* env var but NOT the master gate (charter says
           all forcing must sit behind the single master gate -> still a violation)
  ALLOW    on the explicit platform-fix allowlist below (reviewed; OK, not forcing)
  UNGATED  none of the above -> FLAGGED for review (possible hidden band-aid)

The primitives can't be auto-judged forcing-vs-platform by grep (a writeGuestBE32
can write a fabricated flag OR a real hardware register). So this tool guarantees
the two things a human can't do reliably by eye: COMPLETENESS (no site hides) and
RE-RUNNABILITY. The forcing-vs-platform judgment lives in ALLOWLIST -- small,
explicit, reviewable in five minutes -- everything else must be gated or it flags.

Exit code: 0 if no UNGATED (non-allowlisted) sites, else 1 (usable as a CI gate).

Scanned: kernel/, gfx/, app/ and hand-written guest/*.cpp (iter-463 round 2);
generated guest/ppc/** and the committed splice bodies get the separate
hand-edit check (scan_generated).

iter-463 round 2: gates are judged AT THE WRITE SITE (SiteGates below): every
condition the site needs - enclosing if/else/while, brace-less `if (C) stmt`,
early-exit guards that ALWAYS exit - evaluated to NONE / default-ON / opt-in /
MASTER / dead, with gate predicates read from their own bodies (their real
default), `a || b` taking the weaker and `a && b` the stronger operand. An
unfinished site (no gate in its function) inherits its callers' class.

iter-463 round 4 (D11-D18, see FIX): every spelling of the guest base is
followed (GuestWrites: base[x], getGuestBase()/waitGuestBase()/g_guestBaseTls,
getGlobalGuestMemory out-params, wrapper returns such as kpcr lockWord(), a
pointer parameter some caller feeds a guest pointer, late-assigned pointers,
references) and every write occurrence is its OWN site at its own position;
hand-written guest code (tools/regen/hand: the stubs source and the
PPC_CALL_INDIRECT_FUNC fake returns) is classified; references from generated
code / headers count; generated-code hand edits are judged with SiteGates.
A site tuple is (cls, rel, line, prim, func, detail, code, col).

HEURISTIC LIMITS (stated honestly): a text-level C++ reader, not a compiler.
Not seen as gates (so the site reads MORE ungated - the safe direction):
short-circuit `gate() && write()` inside one expression, ternaries, switch
values, bool flags assigned in loops, gates in callees the site does not test;
an inline `if (!e || e[0] != '0') call()` in a CALLER reads as a kill switch
(D10), not a default-ON gate. Not followed as guest pointers: a pointer stored
in a struct field / container and read back elsewhere, pointers through
function pointers and lambdas called by variable, and a guest pointer returned
through an out-parameter other than getGlobalGuestMemory. A helper that writes
through a FED pointer parameter is judged at each call that passes it a guest
pointer (the body is not a site). Usage:
python scripts/audit_forcing.py [--flagged|--json|--all|--count]
"""
import json
import bisect, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN_DIRS = ["kernel", "gfx", "app"]

# iter-374: guest/ppc needs a DIFFERENT detector, not the same one.
#
# It is generated AND gitignored, so nothing audited it — and an ungated forcing
# site lived there undetected in every build: sub_825F7168 in ppc_recomp.72.cpp
# fabricated a semaphore handle and PPC_STORE_U32'd it into guest memory while
# this script reported "no ungated forcing sites". Gitignored is not harmless;
# hand-patched generated code is exactly where forcing hides, because nobody
# reads it in review.
#
# Running the full function-parser over 144 MB of generated code takes 20+ min,
# which would just get it switched off. But there is a far sharper signal: the
# recompiler NEVER emits host C++. So any reference to our own namespace inside
# guest/ppc is, by construction, a HAND EDIT — and every hand edit must be
# gate-checked. One cheap regex finds all of them.
GENERATED_DIR = os.path.join("guest", "ppc")
HAND_PATCH_RE = re.compile(r"\bsr::kernel::(\w+)")
# Pure OBSERVATION: reads guest state or logs. Present in a hand edit, but does not
# itself force. Counted for visibility, never a failure on its own.
# iter-381d additions: aliasGuestVA/emitGuestTraceEntry/traceIndirectCall are the
# memory-model + tracing INFRA in ppc_context.h (used by every generated site,
# not a hand edit of behavior); dumpHeapState is a log dump.
HAND_PATCH_OBSERVE = re.compile(
    r"^(log\w+|is[A-Z]\w*|get[A-Z]\w*|g_\w+|k[A-Z]\w*|\w+Watchpoint|"
    r"forcingAllowed|naturalModuleInitOn|guestVAToHost|readGuest\w+|setGuestMemory|"
    r"aliasGuestVA|emitGuestTraceEntry|traceIndirectCall|dumpHeapState)$")

# iter-381d: explicit, REVIEWED allowlist for hand edits in GENERATED guest code —
# same contract as the kernel ALLOWLIST below (small, justified, reviewable).
# Match = (file_substr, anchor within ±5 lines of the site, reason).
GENERATED_ALLOWLIST = [
    ("ppc_recomp.109.cpp", "s_dogmaSubOn",
     "RETIRED iter-380: DOGMA alloc substitution, runtime-gated SR_DOGMA_ALLOC_SUB "
     "default-OFF (=1 diagnostic); deletion scheduled after clean sessions"),
    ("ppc_recomp.9.cpp", "s_fmSubOn",
     "RETIRED iter-380b: FM alloc substitution, runtime-gated SR_FM_ALLOC_SUB "
     "default-OFF (=1 diagnostic); deletion scheduled after clean sessions"),
]


def generated_allowlisted(path, line_idx, lines):
    rel = path.replace("\\", "/")
    near = "".join(lines[max(0, line_idx - 5):line_idx + 6])
    splice = SPLICE_BODIES.replace("\\", "/") + "#" in rel
    for fsub, anchor, _reason in GENERATED_ALLOWLIST:
        # a splice body has no ppc_recomp.N.cpp name: the reviewed anchor decides
        if (fsub in rel or splice) and anchor in near:
            return True
    return False
# Primitives that FABRICATE guest state or substitute for the guest's own machinery.
# These must be gated wherever they appear.
HAND_PATCH_FORCING = re.compile(r"^(writeGuest\w+|guestHeapAlloc\w*|alertableYield)$")
# An injected block is host C++ the recompiler never emits. A guest STORE inside one
# is a hand-written write of guest state — exactly the shape of the ungated
# sub_825F7168 fabricator (a `static` counter guarding a PPC_STORE_U32 of a
# semaphore handle the guest never created), which a symbol-name check alone MISSES.
INJECT_MARK_RE = re.compile(r"^\s*(static\s+\w|//\s*(FIX|BANDAID|iter-|HACK)\b)")
GUEST_STORE_RE = re.compile(r"\bPPC_STORE_\w+\s*\(")


SPLICE_BODIES = os.path.join("tools", "regen", "splice_bodies.json")


def _generated_sources(root, source):
    """(label, lines) of generated guest code: the local guest/ppc/ files, or
    (iter-463 round 2) the COMMITTED splice bodies tools/regen/splice_bodies.json
    that tools/regen/assemble.py writes into guest/ppc - checkout-independent,
    so the hand edits are audited even where guest/ppc (gitignored) is absent."""
    if source == "splice":
        p = os.path.join(root, SPLICE_BODIES)
        try:
            bodies = json.load(open(p, encoding="utf-8"))
        except (OSError, ValueError):
            return
        for name in sorted(bodies):
            body = bodies[name]
            if isinstance(body, str):
                yield f"{SPLICE_BODIES}#{name}", body.splitlines(True)
        return
    base = os.path.join(root, GENERATED_DIR)
    if not os.path.isdir(base):
        return
    for name in sorted(os.listdir(base)):
        if not name.endswith((".cpp", ".c", ".h", ".hpp")):
            continue
        path = os.path.join(base, name)
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                yield path, f.readlines()
        except OSError:
            continue


_GEN_FN_CACHE = {}
_GEN_HEADS = [None, None]        # (lines, heads): the PPC_FUNC head indices of the file being scanned
_GEN_HEAD_RE = re.compile(r"\s*(?:PPC_FUNC_IMPL|PPC_FUNC|PPC_WEAK_FUNC)\s*\(")
_WORD_RE = re.compile(r"[A-Za-z_]\w*")
LAST_CTX = {}                    # the AuditCtx of the last collect() (main's generated-code check)


def generated_level(ctx, path, lines, offs, i, col=None):
    """D18: SiteGates level of line i of a generated file / splice body, judged
    inside its enclosing PPC_FUNC body (cached per function). `col` (iter-463
    review) judges a column inside the line - the `return` of a one-line
    `if (gate) { ...; return; }` - instead of its first token."""
    if _GEN_HEADS[0] is not lines:
        _GEN_HEADS[:] = [lines, [k for k, l in enumerate(lines) if _GEN_HEAD_RE.match(l)]]
    heads = _GEN_HEADS[1]
    h = bisect.bisect_right(heads, i) - 1
    lo = heads[h] if h >= 0 else 0
    text_lo = offs[lo]
    key = (path, lo)
    src = _GEN_FN_CACHE.get(key)
    if src is None or src.path not in ctx._src:
        # the function: from its head to the matching close brace. Strip only up
        # to the next head (functions do not nest; the left-to-right strip of a
        # prefix equals the prefix of the strip) - stripping the whole rest of a
        # 6 MB file per function made the full audit take over 40 minutes. The
        # whole tail stays the fallback when the close is not in that window.
        nxt = bisect.bisect_right(heads, lo)
        hi = heads[nxt] if nxt < len(heads) else len(lines)
        tail = "".join(lines[lo:hi])
        clean = _aht.strip_code(tail)
        ob = clean.find("{")
        cb = _brace_close(clean, ob) if ob >= 0 else None
        if cb is None and hi < len(lines):
            tail = "".join(lines[lo:])
            clean = _aht.strip_code(tail)
            ob = clean.find("{")
            cb = _brace_close(clean, ob) if ob >= 0 else None
        body = tail[:cb + 1] if cb is not None else tail
        label = os.path.join(ctx.root, "_gen_", str(len(_GEN_FN_CACHE)))
        src = TextSrc(label, body, ctx.root)
        _GEN_FN_CACHE[key] = src
        ctx._src[label] = src
    pos = offs[i] - text_lo
    if col is not None:
        pos += col
    else:
        m = re.search(r"\S", lines[i])
        pos += m.start() if m else 0
    if pos >= len(src.clean):
        return NONE
    return ctx.gates_site.level_at(src, pos)[0]


def scan_generated(root, source="ppc", ctx=None, writers=()):
    """Report every hand edit in generated guest code, and whether it is gated.

    Returns (sites, ungated) where each site is (path, line, symbol, gated).
    LIMIT: this finds hand-written C++ (sr::kernel:: references, injected stores
    and calls). A hand edit that DELETES generated code (a skipped guest call,
    a forced early return) leaves nothing to match; only a diff against the
    pristine tools/regen/ppc_out shows it."""
    sites, ungated = [], []
    host = set()
    if ctx is not None and FIX["D18"]:
        host = {func_name(sig) for _p, _l, fs in ctx.parsed for _s, _e, sig in fs}
        host = {h for h in host if re.fullmatch(r"[A-Za-z_]\w{2,}", h)
                and not re.match(r"sub_[0-9A-Fa-f]{8}$|__imp__|PPC_", h)}
    for path, lines in _generated_sources(root, source):
        offs = None
        # A hand edit is "gated" if forcingAllowed() appears within a small window
        # above it (same injected block) — deliberately conservative.
        # D18: that window accepted ANY getenv / forcingAllowed text within 25
        # lines - a default-ON switch, one in an unrelated branch, or one AFTER
        # the edit. Now SiteGates judges the edit inside its own function body:
        # gated = opt-in / master / dead; a default-ON switch is not a gate.
        def gated_at(i):
            nonlocal offs
            if ctx is not None and FIX["D18"]:
                if offs is None:
                    offs = [0]
                    for l in lines:
                        offs.append(offs[-1] + len(l))
                return generated_level(ctx, path, lines, offs, i) >= OFF
            lo = max(0, i - 25)
            w = "".join(lines[lo:i + 1])
            return ("forcingAllowed" in w) or ("getenv" in w)

        # Track how far the current injected (hand-written) block extends, so a
        # guest STORE inside one is attributable even when no sr::kernel:: symbol
        # names it.
        inject_until = -1
        for i, line in enumerate(lines):
            if INJECT_MARK_RE.match(line):
                inject_until = i + 12
            for m in HAND_PATCH_RE.finditer(line):
                sym = m.group(1)
                if HAND_PATCH_OBSERVE.match(sym):
                    sites.append((path, i + 1, sym + " [observe]", True))
                    continue
                if generated_allowlisted(path, i, lines):
                    sites.append((path, i + 1, sym + " [allow]", True))
                    continue
                g = gated_at(i)
                sites.append((path, i + 1, sym, g))
                if not g and (HAND_PATCH_FORCING.match(sym) or (FIX["D18"] and sym in writers)):
                    ungated.append((path, i + 1, sym))
            # D18: a host function called WITHOUT sr::kernel:: (the sub_821498F8
            # splice calls walkStaticCtors_impl / walkFmModuleCtors_impl) is a
            # hand edit too - the recompiler never names a host function
            # (the raw-token prefilter is exact: stripping only blanks, and a
            # token left unblanked keeps its raw neighbours, so a line with no
            # host-name token has no host call; it skips stripping every line
            # of the generated tree)
            if host and not host.isdisjoint(_WORD_RE.findall(line)):
                # (on the comment/string-blanked line: a log text "running
                # static-ctor walk (..)" is not a call of walk())
                cline = _aht.strip_code(line)
                for m in re.finditer(r"(?<![\w.>:])([A-Za-z_]\w*)\s*\(", cline):
                    sym = m.group(1)
                    if sym not in host or re.search(r"\bsr\s*::\s*kernel\s*::\s*$", cline[:m.start()]):
                        continue
                    code = line.split("//")[0]
                    if _proto_re(sym).match(code):
                        continue                     # a comment / a declaration
                    g = gated_at(i)
                    sites.append((path, i + 1, sym + " [unqualified]", g))
                    if not g and sym in writers:
                        ungated.append((path, i + 1, sym + " [unqualified]"))
            # A generated store is ALWAYS preceded by its disassembly comment
            # ("// stw r30,1812(r11)"). A hand-written one is not — that single
            # discriminator separates the ~10 real injected writes from the tens of
            # thousands of legitimate generated ones, and keeps this check sharp
            # enough to stay switched on.
            if (i <= inject_until and GUEST_STORE_RE.search(line)
                    and i > 0 and not lines[i - 1].lstrip().startswith("//")
                    # iter-377 (step 0b): the recompiler expands `stwu rN,-K(r1)`
                    # to TWO lines — `ea = -K + ctx.rN.u32;` then the store — so
                    # the store's previous line is not a comment yet the code is
                    # fully generated. 7-8 verified false positives came from
                    # exactly this shape; an `ea =` predecessor is generated.
                    and not re.match(r"\s*ea\s*=", lines[i - 1])
                    # iter-381d: a RUN of generated stores carries ONE disasm
                    # comment ("// stfd f29/f30/f31") — stores 2..N have a store
                    # (or the fpscr float-preamble) as their predecessor. The
                    # FIRST store of a hand-written run still has neither, so
                    # hand runs are still caught by their first store.
                    and not GUEST_STORE_RE.search(lines[i - 1])
                    and "ctx.fpscr." not in lines[i - 1]):
                g = gated_at(i)
                sites.append((path, i + 1, "PPC_STORE in injected block", g))
                if not g:
                    ungated.append((path, i + 1, "PPC_STORE in injected block"))
            # iter-377 (step 0c): raw guest CALLS inside injected blocks — the
            # charter's #1 forcing type (calling a guest function the game did not
            # call), which no store-pattern can see. Same discriminator as stores:
            # a generated call always carries its `// bl 0x…` disasm comment on
            # the line above; a hand-written one does not.
            if (i <= inject_until and i > 0
                    and re.search(r"\bsub_[0-9A-Fa-f]{8}\s*\(\s*ctx\s*,\s*base\s*\)", line)
                    and not lines[i - 1].lstrip().startswith("//")
                    # iter-381d: a generated `bl` expands to TWO lines — the
                    # disasm comment `// bl 0x…`, then `ctx.lr = 0xRETADDR;`,
                    # then the call — so the comment sits at i-2, not i-1. A
                    # nearby injected comment ("// iter-…") arming the window
                    # made 13 pristine bl-pairs flag as hand calls. Requiring
                    # the `// bl` comment at i-2 (with the lr set at i-1) is
                    # exact: hand-written calls never carry the disasm comment.
                    and not (i > 1 and re.match(r"\s*//\s*bl\b", lines[i - 2])
                             and re.match(r"\s*ctx\.lr\s*=", lines[i - 1]))):
                # iter-381d: a call guarded by the named iter-320 gate
                # `if (sr::kernel::naturalModuleInitOn())` on a nearby line is
                # SR-gated even though gated_at's getenv text-scan can't see it.
                lo = max(0, i - 6)
                natGated = any("naturalModuleInitOn" in l for l in lines[lo:i + 1])
                if generated_allowlisted(path, i, lines):
                    sites.append((path, i + 1, "guest CALL in injected block [allow]", True))
                else:
                    g = gated_at(i) or natGated
                    sites.append((path, i + 1, "guest CALL in injected block", g))
                    if not g:
                        ungated.append((path, i + 1, "guest CALL in injected block"))
    return sites, ungated

# The forcing surface: primitives that fabricate guest state or drive guest code.
PRIMS = [
    # iter-463: every writeGuest* helper. The old `writeGuest(?:BE|U)\w+` missed
    # writeGuest32( (43 call sites in stubs.cpp alone, incl. the GPU-IRQ fence
    # fabricator advanceFencesIfStalled).
    ("guest_write", re.compile(r"\bwriteGuest\w*\s*\(")),                    # writeGuestBE32 / writeGuest32 / writeGuestU32BE ...
    ("guest_memset", re.compile(r"\bmemset\s*\(\s*(?:sr::kernel::)?guestVAToHost\b|"
                                r"\bmemset\s*\(\s*g_guestBase\b")),        # bulk write into guest memory
    ("ppc_store", re.compile(r"\bPPC_STORE_\w+\s*\(")),                     # guest store from kernel
    ("guest_call", re.compile(r"\bGuestCaller\b(?!\s*\*)")),               # invoke a guest function from kernel (a GuestCaller* member/param is not a call)
]
# iter-463 round 2 (D7): writes that do not go through a named helper.
#   * memcpy / memmove / memset / strcpy INTO guest memory (the destination is a
#     guest-base expression: g_guestBase + x, guestVAToHost(va), physToHost(..))
#   * a store through a raw cast of such an expression
#     (`*reinterpret_cast<uint32_t*>(g_guestBase + x) = v`, `*guestVAToHost(va) = b`)
#   * (per function, see derived_store_lines) a store through a NON-const pointer
#     variable built from one (`uint8_t* h = guestVAToHost(va); ... h[0] = 1;`,
#     `*h = b`, `h->f = v`, `memcpy(h, ..)`, `storeBE32(h, v)`, Interlocked*(h)).
# LIMITS: a pointer that reaches a write through a struct field, a container, a
# function return other than the listed tokens, or another function's parameter
# is not followed; `base` / `gb` count as the guest base only in guest/*.cpp and
# in functions taking `uint8_t* base` (PPC overrides). Reads (a const pointer,
# memcpy FROM guest memory) are never sites.
GUEST_PTR_TOK = (r"(?:(?:sr::kernel::)?(?:g_guestBase|guestBase|gGuestBase)\b|"
                 r"(?:sr::kernel::)?guestVAToHost\s*\(|(?:\w+\s*(?:\.|->)\s*)?physToHost\s*\(|"
                 r"getGuestPtr\s*\()")
_CAST = r"(?:\(\s*[\w:<>\s]+\*\s*\)\s*|reinterpret_cast\s*<[^;>]*>\s*\(\s*|static_cast\s*<[^;>]*>\s*\(\s*)?"
_ASSIGN = r"\s*(?:<<|>>|[-+*/%&|^])?=(?!=)"
D7_PRIMS = [
    ("guest_memset", re.compile(r"\b(?:std::)?memset\s*\(\s*" + _CAST + GUEST_PTR_TOK)),
    ("guest_memcpy", re.compile(r"\b(?:std::)?(?:mem(?:cpy|move)|str(?:n)?cpy|strcat|s?n?printf)\s*\(\s*"
                                + _CAST + GUEST_PTR_TOK)),
    # (a `*` right after a type name is a pointer DECLARATION, not a deref)
    ("raw_store", re.compile(r"(?<![\w>\]])(?<![\w>\]]\s)\*\s*" + _CAST + r"\(?\s*" + GUEST_PTR_TOK +
                             r"[^;=!<>]*?\)?\s*\)?" + _ASSIGN + r"|"
                             r"reinterpret_cast\s*<[^;>]*>\s*\(\s*" + GUEST_PTR_TOK + r"[^;!<>]*?\)\s*\[[^\]]*\]" + _ASSIGN)),
]


def all_prims():
    """The line-level primitive set in force (D7 adds the raw-write forms)."""
    return list(PRIMS) + (D7_PRIMS if FIX["D7"] else [])


_DERIVED_DECL = re.compile(
    r"(?<![\w:])(const\s+)?(?:[A-Za-z_][\w:]*(?:\s*<[^;=()]*>)?)\s*\*\s*(?:const\s+|__restrict\s+)?(\w+)\s*=\s*"
    r"|(?<![\w:])(const\s+)?auto\s+(\w+)\s*=\s*")


def derived_store_lines(src, lo, hi, base_ok):
    """{line_idx: kind} of stores through non-const pointers derived from the
    guest base inside src.clean[lo:hi] (one function). Scoped: the store must
    see a derived declaration of that name (declared before it, in a block that
    encloses it) with no lambda parameter of the same name in between."""
    c = src.clean
    tok = GUEST_PTR_TOK + (r"|\bbase\b|\bgb\b" if base_ok else "")
    tok_re = re.compile(tok)
    decls = []                                       # (name, init, pos, block)
    for m in _DERIVED_DECL.finditer(c, lo, hi):
        if m.group(1) or m.group(3):
            continue                                 # pointer to const: read-only
        name = m.group(2) or m.group(4)
        end = c.find(";", m.end(), hi)
        # an if-condition declaration ends at its ')' - the init is short either way
        init = c[m.end():end if end >= 0 else hi]
        decls.append((name, init, m.start(), src.innermost(m.start())))
    derived = {}                                     # name -> [(pos, block)]
    changed = True
    while changed:                                   # p = q + 4 where q is derived
        changed = False
        for name, init, pos, blk in decls:
            if (pos, blk) in derived.get(name, []):
                continue
            if tok_re.search(init) or any(re.search(r"(?<![\w.>])" + re.escape(d) + r"\b(?!\s*(?:\[|->))", init)
                                          for d in derived):
                derived.setdefault(name, []).append((pos, blk))
                changed = True
    # lambda parameter lists: `[..](uint8_t* h, ..) { .. }` shadows h inside
    lam_params = []
    for m in re.finditer(r"\]\s*\(([^()]*)\)\s*(?:mutable\s*)?(?:->\s*[\w:<>\*&\s]+?)?\{", c[lo:hi]):
        ob = lo + m.end() - 1
        cb = src.brace_match.get(ob)
        if cb is not None:
            lam_params.append((set(re.findall(r"(\w+)\s*(?:=[^,]*)?(?:,|$)", m.group(1))), ob, cb))
    out = {}
    for v, where in derived.items():
        vr = re.escape(v)
        rx = re.compile(
            r"(?<![\w.>])" + vr + r"\s*\[[^\]]*\]" + _ASSIGN + r"|"
            r"(?:^|(?<=[;{})])|(?<=\belse))\s*\*\s*" + _CAST + r"\(?\s*" + vr + r"\b[^;=()!<>]*\)?\s*\)?" + _ASSIGN + r"|"
            r"(?<![\w.>])" + vr + r"\s*->\s*\w+(?:\s*\[[^\]]*\])?" + _ASSIGN + r"|"
            r"\b(?:std::)?(?:mem(?:cpy|move|set)|str(?:n)?cpy|strcat|s?n?printf)\s*\(\s*" + _CAST + vr + r"\b|"
            r"\b(?:store|write|wr|put)\w*\s*\(\s*" + _CAST + vr + r"\b|"
            r"\b_?Interlocked\w*\s*\(\s*" + _CAST + vr + r"\b", re.M)
        for m in rx.finditer(c, lo, hi):
            q = m.start() + len(m.group(0)) - len(m.group(0).lstrip())
            chain = src.enclosing(q)
            seen = [(pos, blk) for pos, blk in where if pos < q and (blk == -1 or blk in chain)]
            if not seen:
                continue                             # no derived declaration in scope
            dpos = max(pos for pos, _b in seen)
            if any(v in names and dpos < ob < q <= cb for names, ob, cb in lam_params):
                continue                             # a lambda parameter shadows it
            out.setdefault(src.line_of(q) - 1, "raw_store")
    return out


# ---------------------------------------------------------------------------
# iter-463 round 4 (D11): the GUEST-POINTER MODEL. Round 2's D7 followed only
# pointers DECLARED from a guest-base token in the same function, and only
# `base`/`gb` in guest/*.cpp or `uint8_t* base` overrides. The verifier found at
# least 17 real guest writes that were no site at all (fm_module_walker's direct
# `base[x] = 0`, kpcr's spinlock word through lockWord(), fe_flow_watch's `gb`
# thread parameter, ...). The model below follows every spelling of the guest
# base:
#   roots     g_guestBase / guestBase / g_guestBaseTls, guestVAToHost(..),
#             getGuestBase(), waitGuestBase(), getGuestPtr(..), PPC_GUEST_BASE(),
#             physToHost(..); `base` / `gb` in hand-written guest code
#   params    a pointer parameter named base / gb / gbase / guestBase / gBase, and
#             ANY pointer parameter some call site (or std::thread(fn, ..)) feeds
#             a guest pointer (fixpoint over the tree)
#   out-param getGlobalGuestMemory(&x, ..)
#   wrappers  a pointer-returning function that returns a guest pointer
#             (kpcr lockWord) becomes a root itself (fixpoint)
#   derived   `T* x = e`, `T* x(e)`, `auto x = e`, `T& r = *e`, and a LATER
#             `x = e` - any non-const pointer / reference built from the above
# and finds every WRITE through one, each at its own position: `x[i] op= v`,
# `*x = v`, `*(T*)(x + k) = v`, `x->a.b[i] = v`, `r.f = v`, `++*x`, `x[i]++`,
# `x->store(..)`, and x as the DESTINATION argument of memcpy / memset / str*cpy /
# *printf / *_s / ZeroMemory / CopyMemory / FillMemory / fread / std::fill(_n) /
# std::copy(_n) / Interlocked* / store*/write*/wr*/put* helpers. Reads (a const
# pointer, x as a SOURCE argument, `v = x[i]`) are never sites.
GUEST_ROOT_RE = re.compile(
    r"(?<![\w.>])(?:::)?(?:sr\s*::\s*kernel\s*::\s*)?(?:g_guestBase|gGuestBase|guestBase|g_guestBaseTls)\b(?!\s*\()"
    r"|(?<![\w.>])(?:::)?(?:sr\s*::\s*kernel\s*::\s*)?(?:guestVAToHost|getGuestBase|waitGuestBase|getGuestPtr)\s*\("
    r"|(?<![\w.>])PPC_GUEST_BASE\s*\("
    r"|(?<!\w)(?:\w+\s*(?:\.|->)\s*)?physToHost\s*\(")
_HAND_ROOT_RE = re.compile(r"(?<![\w.>])(?:base|gb)\b(?!\s*\()")
# hand-written guest code: `base` / `gb` ARE the guest base there (D7 / D16)
HAND_GUEST_PREFIXES = ("guest/", "tools/regen/hand/")
_OUT_PARAM_RE = re.compile(r"\bgetGlobalGuestMemory\s*\(\s*&\s*(\w+)")
_BASE_PARAMS = {"base", "gb", "gbase", "guestBase", "gBase"}
_ATOMIC_WRITE = {"store", "exchange", "fetch_add", "fetch_sub", "fetch_or", "fetch_and", "fetch_xor",
                 "compare_exchange_weak", "compare_exchange_strong"}
_FILL_DEST0 = {"memset", "ZeroMemory", "RtlZeroMemory", "SecureZeroMemory", "FillMemory", "RtlFillMemory",
               "fill", "fill_n", "uninitialized_fill", "uninitialized_fill_n", "bzero"}
_COPY_DEST0 = {"memcpy", "memmove", "strcpy", "strncpy", "strcat", "strncat", "sprintf", "snprintf",
               "vsprintf", "vsnprintf", "_snprintf", "_vsnprintf", "memcpy_s", "memmove_s", "strcpy_s",
               "strncpy_s", "strcat_s", "strncat_s", "sprintf_s", "snprintf_s", "_snprintf_s",
               "vsprintf_s", "vsnprintf_s", "_vsnprintf_s", "wcscpy", "wcsncpy", "wcscpy_s", "wcsncpy_s",
               "swprintf", "swprintf_s", "CopyMemory", "RtlCopyMemory", "MoveMemory", "RtlMoveMemory",
               "fread", "fgets", "generate", "generate_n", "iota", "atomic_store", "atomic_exchange",
               "atomic_store_explicit", "atomic_exchange_explicit", "atomic_fetch_add", "atomic_fetch_sub"}
_COPY_DESTN = {"copy": 2, "copy_n": 2, "copy_backward": 2, "copy_if": 2, "move_backward": 2,
               "reverse_copy": 2, "replace_copy": 2, "ReadFile": 1}
_CASTS = {"reinterpret_cast", "static_cast", "const_cast", "dynamic_cast", "bit_cast"}
_CTRL = {"if", "while", "for", "switch", "return", "sizeof", "decltype", "alignof", "catch",
         "alignas", "noexcept", "static_assert", "typeid", "co_return", "co_await", "co_yield"}
_UNARY_KW = {"return", "else", "do", "case", "throw", "co_return", "co_yield"}
_CTYPE_RE = re.compile(r"\s*(?:(?:const|volatile|unsigned|signed|struct|class)\s+)*[A-Za-z_][\w:]*"
                       r"(?:\s*<[^()]*>)?(?:\s+(?:const|volatile))?\s*[\*&]+\s*(?:(?:const|volatile)\s*)*")
_ASSIGN_AT = re.compile(r"(?:<<|>>|[-+*/%&|^])?=(?!=)")
_PTR_PARAM_RE = re.compile(r"[\*&]")


def _prev_ns(c, i):
    """Index of the last non-space char before i (or -1)."""
    i -= 1
    while i >= 0 and c[i].isspace():
        i -= 1
    return i


def _match_close(c, o, pair="()"):
    op, cl = pair
    depth = 0
    for i in range(o, len(c)):
        if c[i] == op:
            depth += 1
        elif c[i] == cl:
            depth -= 1
            if depth == 0:
                return i
    return None


def _match_open(c, close, pair="()"):
    op, cl = pair
    depth = 0
    for i in range(close, -1, -1):
        if c[i] == cl:
            depth += 1
        elif c[i] == op:
            depth -= 1
            if depth == 0:
                return i
    return None


def _word_before(c, k):
    """(word, start) of the identifier ending at index k (inclusive), or ("", k+1)."""
    j = k
    while j >= 0 and (c[j].isalnum() or c[j] == "_"):
        j -= 1
    return c[j + 1:k + 1], j + 1


def _owner(c, o):
    """What the '(' at o belongs to: ("call", name, start) / ("cast", "", start) /
    ("ctrl", kw, start) / ("group", "", o)."""
    k = _prev_ns(c, o)
    if k < 0:
        return "group", "", o
    if c[k] == ">":                                   # template args: cast<T*>( / f<T>(
        depth, j = 0, k
        while j >= 0:
            if c[j] == ">":
                depth += 1
            elif c[j] == "<":
                depth -= 1
                if depth == 0:
                    break
            j -= 1
        if j < 0:
            return "group", "", o
        k = _prev_ns(c, j)
        w, ws = _word_before(c, k)
        if w in _CASTS:
            q = ws
            while q >= 2 and c[q - 2:q] == "::":       # std::bit_cast
                _w, q = _word_before(c, q - 3)
            return "cast", w, q
        return ("call", w, ws) if w else ("group", "", o)
    if c[k] == ")":                                   # (T*)(expr): a C-style cast
        oo = _match_open(c, k)
        if oo is not None and _CTYPE_RE.fullmatch(c[oo + 1:k]):
            return "cast", "", oo
        return "group", "", o
    if c[k].isalnum() or c[k] == "_":
        w, ws = _word_before(c, k)
        if w in _CTRL:
            return "ctrl", w, ws
        pk = _prev_ns(c, ws)
        if pk >= 0 and (c[pk] == "." or c[pk - 1:pk + 1] == "->"):
            return "method", w, ws                     # obj.store(x): not a free function
        return "call", w, ws                           # (the NAME: std::memcpy -> memcpy)
    return "group", "", o


def _is_unary(c, j):
    """c[j] is '*' or '&': unary (deref / address-of), not binary / a declarator."""
    if c[j] == "&" and (c[j - 1:j] == "&" or c[j + 1:j + 2] == "&"):
        return False
    k = _prev_ns(c, j)
    if k < 0:
        return True
    ch = c[k]
    if ch in "(,=;{}[!~?:|^+-*/%<&":
        return True
    if ch == ">":
        # `T<U>* x` (a declarator) vs `a >> *p`; a bare `a > *p` comparison is
        # rare next to template declarators, so it reads as a declarator
        return c[k - 1:k] == ">" and c[k - 2:k - 1] != ">"
    if ch == ")":
        oo = _match_open(c, k)
        if oo is None:
            return False
        kind, w, _s = _owner(c, oo)
        return kind == "ctrl" and w in ("if", "while", "for", "switch")
    if ch.isalnum() or ch == "_":
        w, _s = _word_before(c, k)
        return w in _UNARY_KW
    return False


def _arg_span(c, o, pos):
    """(index, start, end) of the argument of the call / group opened at o that
    contains pos (top-level commas)."""
    close = _match_close(c, o)
    if close is None:
        return None
    idx, start, depth, i = 0, o + 1, 0, o + 1
    while i < close:
        ch = c[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "<" and depth == 0:
            pass
        elif ch == "," and depth == 0:
            if pos < i:
                return idx, start, i, close
            idx, start = idx + 1, i + 1
        i += 1
    return idx, start, close, close


def _nargs(c, o):
    close = _match_close(c, o)
    if close is None:
        return 0
    if not c[o + 1:close].strip():
        return 0
    n, depth = 1, 0
    for i in range(o + 1, close):
        if c[i] in "([{":
            depth += 1
        elif c[i] in ")]}":
            depth -= 1
        elif c[i] == "," and depth == 0:
            n += 1
    return n


def _call_dest_kind(name, idx, nargs):
    """Write kind when argument idx of a call to `name` is a DESTINATION."""
    if name in _FILL_DEST0 and idx == 0:
        return "guest_memset"
    if name in _COPY_DEST0 and idx == 0:
        return "guest_memcpy"
    if name in _COPY_DESTN and idx == _COPY_DESTN[name]:
        return "guest_memcpy"
    if name == "move" and nargs == 3 and idx == 2:
        return "guest_memcpy"
    if name == "transform" and idx == (2 if nargs == 4 else 3):
        return "guest_memcpy"
    if idx == 0 and (re.match(r"(?:store|write|wr|put)[A-Z0-9_]", name + "_")
                     or re.match(r"_?Interlocked\w*$", name) or name in ("swap", "exchange")):
        return "raw_store"
    if idx == 1 and name == "swap":
        return "raw_store"
    return None


def dest_write(c, s, e, ref=False, depth=0):
    """(kind, write position) when the guest-pointer expression c[s:e] (a root
    token, or a derived name; ref = it NAMES guest memory) is the destination of
    a write; None for a read / a pointer copy / anything not understood."""
    deref = ref
    wpos = s
    for _ in range(40):
        # postfix chain: [i]  ->m  .m  ->m(..) .m(..)
        while True:
            k = _ws(c, e)
            if c.startswith("[", k):
                close = _match_close(c, k, "[]")
                if close is None:
                    return None
                e, deref = close + 1, True
                continue
            m = re.match(r"(->|\.)\s*(~?[A-Za-z_]\w*)", c[k:k + 80]) if not c.startswith("...", k) else None
            if m:
                after = _ws(c, k + m.end())
                if c.startswith("(", after):
                    arrow = m.group(1) == "->"
                    if m.group(2) in _ATOMIC_WRITE and (arrow or deref):
                        return "raw_store", wpos
                    return None                        # a method call on it
                if m.group(1) == "." and not deref:
                    return None                        # a member of a pointer VALUE (p.x): not guest memory
                e, deref = k + m.end(), True
                continue
            break
        k = _ws(c, e)
        if c.startswith("++", k) or c.startswith("--", k):
            if deref:
                return "raw_store", wpos
            e = k + 2                                   # `p++` is the pointer itself: *p++ reads
            continue
        if _ASSIGN_AT.match(c, k) and not c.startswith("==", k):
            if deref:
                return "raw_store", wpos
        j = _prev_ns(c, s)
        if j < 0:
            return None
        if j >= 1 and c[j - 1:j + 1] in ("++", "--"):
            return ("raw_store", wpos) if deref else None
        if c[j] == "*" and _is_unary(c, j):
            if deref and not ref:
                pass                                   # **pp: still guest memory
            s, deref, wpos = j, True, j
            continue
        if c[j] == "&" and _is_unary(c, j):
            s, deref, wpos = j, False, j
            continue
        if c[j] == ")":
            oo = _match_open(c, j)
            if oo is not None and _CTYPE_RE.fullmatch(c[oo + 1:j]) and _owner(c, oo)[0] == "group":
                s, wpos = oo, oo                        # (T*)x
                continue
            return None
        if c[j] in "(,":
            o = j if c[j] == "(" else None
            if o is None:                               # find the enclosing '('
                depth_, i = 0, j - 1
                while i >= 0:
                    if c[i] in ")]}":
                        depth_ += 1
                    elif c[i] in "([{":
                        if depth_ == 0:
                            break
                        depth_ -= 1
                    i -= 1
                if i < 0 or c[i] != "(":
                    return None
                o = i
            span = _arg_span(c, o, s)
            if span is None:
                return None
            idx, a0, a1, close = span
            before, after = c[a0:s].strip(), c[e:a1].strip()
            if before:
                return None                             # `k + x`, `f(y, x)` nested - not the pointer core
            arith = bool(after) and re.match(r"[-+](?![->=+])", after) is not None
            if after and not arith:
                return None
            if arith and deref:
                return None                             # (x[i] + 1): a VALUE
            kind, name, ostart = _owner(c, o)
            if kind in ("cast", "group"):
                if _nargs(c, o) != 1:
                    return None
                s, e, wpos = ostart, close + 1, ostart
                continue
            if kind == "call" and not deref:
                w = _call_dest_kind(name, idx, _nargs(c, o))
                return (w, ostart) if w else None
            return None
        return None
    return None


def _params_of(sig):
    """[(name, type_text)] of the parameter list of a parsed function signature."""
    m = re.search(r"(\w+)\s*\((.*)\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->[^{]*)?\{?\s*$", sig, re.S)
    if not m:
        return []
    inner = m.group(2)
    out, depth, cur = [], 0, ""
    for ch in inner:
        if ch in "(<[":
            depth += 1
        elif ch in ")>]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    res = []
    for p in out:
        p = p.split("=")[0].strip()
        nm = re.search(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?$", p)
        if not nm or p == nm.group(1):
            res.append(("", p))
            continue
        res.append((nm.group(1), p[:nm.start()].strip()))
    return res


class GuestWrites:
    """D11: tree-wide guest-pointer model. .sites(path) -> {pos: kind}.

    Every derived name carries its ORIGINS: "root" (built from the guest base
    in this function, or a base / gb parameter) or ("p", i) (built only from
    pointer parameter i, which some caller feeds a guest pointer). A write
    through a root-origin name is a site where it stands. A write through a
    parameter-origin name makes the function a WRITER of that parameter, and
    the site is each CALL that passes it a guest pointer - so a generic helper
    (writeBE32(uint8_t* p, ..)) is judged at every call with that call's gate,
    never once in its body with the weakest caller's."""

    def __init__(self, ctx):
        self.ctx = ctx
        self.fns = []                   # (path, src, ob, cb, name, sig, hand)
        for path, _lines, funcs in ctx.parsed:
            src = ctx.src(path)
            hand = ctx.rel(path).startswith(HAND_GUEST_PREFIXES)
            for fs, fe, sig in funcs:
                ob = self._open_brace(src, fs, fe)
                if ob is None:
                    continue
                self.fns.append((path, src, ob, src.brace_match[ob], func_name(sig), sig, hand))
        self.by_name = {}
        for i, f in enumerate(self.fns):
            self.by_name.setdefault(f[4], []).append(i)
        self.params = {i: _params_of(f[5]) for i, f in enumerate(self.fns)}
        self.param_seed = {}            # fn index -> {param name: origin}
        self.wrappers = set()           # names of functions returning a guest pointer
        self.writes_param = {}          # fn name -> {param index} it writes through
        for i, f in enumerate(self.fns):
            seeds = {}
            for j, (nm, ty) in enumerate(self.params[i]):
                # the guest-base CONVENTION: a byte pointer named base / gb / ..
                # (a `uint32_t* base` out-param is not the guest base)
                if nm in _BASE_PARAMS and re.fullmatch(
                        r"(?:u?int8_t|unsigned\s+char|char|u8|void|std::uint8_t)\s*\*\s*(?:__restrict_*\s*)?", ty.strip()):
                    seeds[nm] = "root"
            self.param_seed[i] = seeds
        self._res = {}
        self._fixpoint()

    @staticmethod
    def _open_brace(src, fs, fe):
        ls = src._line_starts
        lo = ls[fs]
        hi = ls[fs + 1] if fs + 1 < len(ls) else len(src.clean)
        end_lo = ls[fe]
        end_hi = ls[fe + 1] if fe + 1 < len(ls) else len(src.clean)
        for i in range(lo, hi):
            if src.clean[i] == "{" and i in src.brace_match and end_lo <= src.brace_match[i] < end_hi:
                return i
        return None

    def _root_re(self):
        if not self.wrappers:
            return GUEST_ROOT_RE
        alt = "|".join(sorted(map(re.escape, self.wrappers), key=len, reverse=True))
        return re.compile(GUEST_ROOT_RE.pattern + r"|(?<![\w.>])(?:\w+\s*::\s*)*(?:" + alt + r")\s*\(")

    def _fixpoint(self):
        dirty = set(range(len(self.fns)))
        for _it in range(12):
            if not dirty:
                break
            root = self._root_re()
            new_names, new_seed = set(), set()
            for i in sorted(dirty):
                f = self.fns[i]
                r = self._analyze(i, root)
                self._res[i] = r
                if r["returns_guest"] and f[4] not in self.wrappers and re.search(
                        r"\*|\bauto\b", f[5][:max(0, f[5].find(f[4] + "("))] if f[4] + "(" in f[5] else ""):
                    self.wrappers.add(f[4])
                    new_names.add(f[4])
                wp = self.writes_param.setdefault(f[4], set())
                if not r["writes_param"] <= wp:
                    wp |= r["writes_param"]
                    new_names.add(f[4])
                for callee, idx in r["feeds"]:
                    for j in self.by_name.get(callee, []):
                        ps = self.params[j]
                        if idx < len(ps):
                            nm, ty = ps[idx]
                            if nm and _PTR_PARAM_RE.search(ty) and not re.match(r"\s*const\b", ty) \
                                    and nm not in self.param_seed[j]:
                                self.param_seed[j][nm] = ("p", idx)
                                new_seed.add(j)
            # next round: only functions whose inputs changed (a new seed, or a
            # call to a function that became a wrapper / a parameter writer)
            dirty = set(new_seed)
            if new_names:
                rx = re.compile(r"\b(?:" + "|".join(map(re.escape, sorted(new_names))) + r")\s*\(")
                for i, f in enumerate(self.fns):
                    if rx.search(f[1].clean, f[2], f[3]):
                        dirty.add(i)

    # ---- one function
    def _analyze(self, i, root):
        path, src, ob, cb, fname, sig, hand = self.fns[i]
        c = src.clean
        empty = {"writes": {}, "feeds": set(), "returns_guest": False, "writes_param": set()}
        if PRIMS[0][1].match(fname + "(") or fname in ("guestVAToHost", "getGuestBase", "waitGuestBase"):
            return empty                                # a primitive helper: its CALLS are the sites
        tokens = []                                     # (s, e) root occurrences
        for m in root.finditer(c, ob, cb):
            e = m.end()
            if c[e - 1] == "(":
                close = _match_close(c, e - 1)
                if close is None:
                    continue
                e = close + 1
            tokens.append((m.start(), e))
        if hand:
            tokens += [(m.start(), m.end()) for m in _HAND_ROOT_RE.finditer(c, ob, cb)]
        derived = {}                                    # name -> [(pos, blk, ref, origins)]
        for nm, org in self.param_seed[i].items():
            derived.setdefault(nm, []).append((ob, ob, False, frozenset([org])))
        if not tokens and not derived and not _OUT_PARAM_RE.search(c, ob, cb):
            return empty
        defs = self._defs(src, ob, cb)
        lam = self._lambda_params(src, ob, cb)

        def visible(name, q):
            for pos, blk, ref, org in reversed(derived.get(name, [])):
                if pos <= q and (blk == -1 or blk == ob or blk in src.enclosing(q)):
                    if any(name in names and pos < lo < q <= hi for names, lo, hi in lam):
                        continue
                    return True, ref, org
            return False, False, frozenset()

        def origins(a, b, any_pos=False):
            out = set()
            for s, e in tokens:
                if a <= s < b and (any_pos or self._ptr_pos(c, s, e)):
                    out.add("root")
                    break
            for nm in list(derived):
                for m in re.finditer(r"(?<![\w.>])" + re.escape(nm) + r"\b(?!\s*\()", c[a:b]):
                    q = a + m.start()
                    ok, _ref, org = visible(nm, q)
                    if ok and (any_pos or self._ptr_pos(c, q, q + len(nm))):
                        out |= org
            return frozenset(out)

        for _k in range(12):
            changed = False
            for name, a, b, pos, blk, kind in defs:
                org = frozenset(["root"]) if kind == "out" else origins(a, b, any_pos=(kind == "ref"))
                if not org:
                    continue
                prev = [e for e in derived.get(name, []) if e[0] == pos]
                if prev and org <= prev[0][3]:
                    continue
                if prev:
                    derived[name].remove(prev[0])
                    org = org | prev[0][3]
                derived.setdefault(name, []).append((pos, blk, kind == "ref", org))
                derived[name].sort(key=lambda e: e[0])
                changed = True
            if not changed:
                break
        # writes
        writes, writes_param = {}, set()
        for s, e in tokens:
            r = dest_write(c, s, e)
            if r:
                writes.setdefault(r[1], r[0])
        if derived:
            alt = "|".join(sorted(map(re.escape, derived), key=len, reverse=True))
            for m in re.finditer(r"(?<![\w.>])(?:" + alt + r")\b(?!\s*\()", c[ob:cb]):
                q = ob + m.start()
                ok, ref, org = visible(m.group(0), q)
                if not ok or self._is_decl_name(c, q):
                    continue
                r = dest_write(c, q, q + len(m.group(0)), ref)
                if r:
                    if "root" in org:
                        writes.setdefault(r[1], r[0])
                    writes_param |= {o[1] for o in org if o != "root"}
        # guest pointers fed to calls (param derivation / writer calls) and
        # returned (wrappers)
        feeds = set()
        for m in re.finditer(r"(?<![\w.>])((?:\w+\s*::\s*)*)([A-Za-z_]\w*)\s*\(", c[ob:cb]):
            name = m.group(2)
            if name in _CTRL or name in _CASTS:
                continue
            o = ob + m.end() - 1
            close = _match_close(c, o)
            if close is None:
                continue
            args, depth_, st = [], 0, o + 1
            for k in range(o + 1, close + 1):
                ch = c[k]
                if ch in "([{":
                    depth_ += 1
                elif ch in ")]}":
                    if depth_ == 0:
                        args.append((st, k))
                        break
                    depth_ -= 1
                elif ch == "," and depth_ == 0:
                    args.append((st, k))
                    st = k + 1
            shift, callee = 0, name
            if name == "thread" and args:                # std::thread(fn, a, b): fn's params
                callee = re.sub(r"^[&\s]*(?:\w+::)*", "", c[args[0][0]:args[0][1]]).strip()
                shift = 1
                if not re.fullmatch(r"[A-Za-z_]\w*", callee):
                    continue
            wp = self.writes_param.get(callee, set())
            for idx, (a, b) in enumerate(args[shift:]):
                if re.match(r"\s*&", c[a:b]):
                    continue
                org = origins(a, b)
                if not org:
                    continue
                feeds.add((callee, idx))
                if idx in wp:
                    # a call handing a guest pointer to a function that writes
                    # through that parameter: the WRITE happens here
                    if "root" in org:
                        writes.setdefault(ob + m.start(2) if shift == 0 else ob + m.start(2), "raw_store")
                    writes_param |= {x[1] for x in org if x != "root"}
        returns_guest = False
        for m in re.finditer(r"\breturn\b", c[ob:cb]):
            r = ob + m.end()
            end = c.find(";", r, cb)
            if end > r and origins(r, end):
                returns_guest = True
                break
        return {"writes": writes, "feeds": feeds, "returns_guest": returns_guest,
                "writes_param": writes_param}

    @staticmethod
    def _ptr_pos(c, s, e):
        """The occurrence is used as a POINTER (not dereferenced to a value)."""
        k = _ws(c, e)
        j = _prev_ns(c, s)
        if j >= 0 and c[j] == "&" and _is_unary(c, j):
            return True
        if c.startswith("[", k) or c.startswith("->", k):
            return False
        if j >= 0 and c[j] == "*" and _is_unary(c, j):
            return False
        return True

    @staticmethod
    def _is_decl_name(c, q):
        """q starts the declarator name of a declaration (`T* q = ..`)."""
        j = _prev_ns(c, q)
        if j < 0:
            return False
        if c[j] in "*&":
            k = _prev_ns(c, j)
            while k >= 0 and c[k] in "*&":
                k = _prev_ns(c, k)
            if k >= 0 and (c[k].isalnum() or c[k] in "_>"):
                w, _s = _word_before(c, k)
                return w not in _UNARY_KW
        if c[j].isalnum() or c[j] == "_" or c[j] == ">":
            w, _s = _word_before(c, j)
            return bool(w) and w not in _UNARY_KW and not w.isdigit()
        return False

    _DECL_RE = re.compile(
        r"(?<![\w:.>])(const\s+)?(?:(?:static|volatile|thread_local|constexpr|inline)\s+)*"
        r"(?:auto|[A-Za-z_][\w:]*(?:\s*<[^;=(){}]*>)?)\s*(?:(?:const|volatile)\s*)?([\*&]+)?\s*"
        r"(?:const\s+|__restrict\s+|__restrict__\s+)?([A-Za-z_]\w*)\s*(=(?!=)|\((?!\s*\))|\{(?!\s*\}))")

    def _defs(self, src, ob, cb):
        """[(name, init_a, init_b, pos, block, kind)] of candidate derivations."""
        c = src.clean
        out = []
        for m in self._DECL_RE.finditer(c, ob, cb):
            const, star, name, op = m.group(1), m.group(2), m.group(3), m.group(4)
            head = c[m.start():m.start(3)]
            is_auto = re.match(r"\s*(?:const\s+)?(?:(?:static|volatile|thread_local|constexpr|inline)\s+)*auto\b", head)
            if const or name in _CTRL or name in _UNARY_KW:
                continue
            tname = re.match(r"\s*(?:(?:static|volatile|thread_local|constexpr|inline)\s+)*([A-Za-z_][\w:]*)", head)
            if tname and tname.group(1) in _CTRL | _UNARY_KW | {"delete", "new", "goto", "using", "typedef"}:
                continue
            if not star and not is_auto:
                continue
            if re.search(r"\bconst\b", head) and (not star or head.rfind("const") < head.rfind(star[0])):
                continue                                 # pointer / reference to const
            kind = "ref" if star and "&" in star else "ptr"
            if op == "=":
                a = m.end()
                b = self._expr_end(c, a, cb)
            else:
                a = m.end()
                b = _match_close(c, m.end() - 1, "()" if op == "(" else "{}")
                if b is None:
                    continue
                if op == "(" and not re.search(r"[\w\)\]]", c[a:b]):
                    continue
            out.append((name, a, b, m.start(3), src.innermost(m.start(3)), kind))
        # later assignments `x = e` (statement start) and out-params
        for m in re.finditer(r"(?:(?<=[;{}(,])|(?<=\belse)|(?<=\bdo)|^)\s*([A-Za-z_]\w*)\s*=(?!=)", c[ob:cb], re.M):
            name = m.group(1)
            if name in _CTRL or name in _UNARY_KW:
                continue
            q = ob + m.start(1)
            a = ob + m.end()
            b = self._expr_end(c, a, cb)
            blk = ob
            for dm in re.finditer(r"[\w>\*&]\s+[\*&]*\s*" + re.escape(name) + r"\s*[=;,({\[]", c[ob:q]):
                blk = src.innermost(ob + dm.start())
            out.append((name, a, b, q, blk, "assign"))
        for m in _OUT_PARAM_RE.finditer(c, ob, cb):
            out.append((m.group(1), m.start(), m.end(), m.start(1), ob, "out"))
        return out

    @staticmethod
    def _expr_end(c, a, limit):
        depth = 0
        for k in range(a, limit):
            ch = c[k]
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                if depth == 0:
                    return k
                depth -= 1
            elif (ch == ";" or ch == ",") and depth == 0:
                return k
        return limit

    @staticmethod
    def _lambda_params(src, ob, cb):
        c, out = src.clean, []
        for m in re.finditer(r"\]\s*\(([^()]*)\)\s*(?:mutable\s*)?(?:->\s*[\w:<>\*&\s]+?)?\{", c[ob:cb]):
            o = ob + m.end() - 1
            close = src.brace_match.get(o)
            if close is not None:
                names = set(re.findall(r"(\w+)\s*(?:=[^,]*)?(?:,|$)", m.group(1)))
                out.append((names, o, close))
        return out

    def sites(self, path):
        """{pos: kind} of guest writes in the file."""
        out = {}
        for i, f in enumerate(self.fns):
            if f[0] == path:
                for pos, kind in self._res.get(i, {}).get("writes", {}).items():
                    out.setdefault(pos, kind)
        return out


# iter-463: host-sync primitives - a host ReleaseSemaphore / SetEvent on a
# guest-visible object is a release the guest may never have made (the
# DOGMA_AUTORELEASE / GPU_SEM auto-track class). Scanned in kernel/ only (gfx/
# and app/ own host-only objects). A registered export whose NAME is itself a
# signal operation (NtReleaseSemaphore, KeSetEvent, NtReadFile's completion
# event, ...) is implementing the guest's own call: see EXPORT_SIGNAL_RE.
HOST_SYNC_PRIM = ("host_sync", re.compile(r"\b(?:ReleaseSemaphore|SetEvent)\s*\("))
HOST_SYNC_DIRS = ("kernel/",)
# An export implementation (registered via registerExport) writing guest memory
# is the console's kernel filling its own OUT parameters / objects - platform,
# the guest made the call. Signal primitives are only OK inside exports whose
# contract IS a signal (the guest asked for exactly that release/set).
EXPORT_SIGNAL_RE = re.compile(
    r"(Release\w*Semaphore|SetEvent|PulseEvent|ReadFile|WriteFile|"
    r"DeviceIoControl|FsControlFile|SignalAndWait)", re.I)
# The master gate OR'd with anything else is NOT the master gate: the other
# operand runs the forcing with SR_ALLOW_FORCING off (DOGMA_AUTORELEASE's
# `s_autorelease || forcingAllowed()`, the pre-iter-463 GPU_SEM auto-track's
# `own || forcingAllowed()`). Such sites are DEFAULT-ON own-gated, not MASTER.
OR_BYPASS_RE = re.compile(
    r"(?:\|\|\s*(?:sr::kernel::)?forcingAllowed\s*\(|forcingAllowed\s*\(\s*\)\s*\|\|)")

# iter-463 audit-bug fixes B1..B7 (manual review of the 55 DEFAULT-ON sites: 27
# were master-gated forcing and 28 platform code, all misread by these bugs):
#   B1 gate helpers resolved by bare name across the tree (watchEnabled, enabled)
#   B2 first matching helper won, so an own-gate beat the real master helper
#   B3 `if (!forcingAllowed() || !own) <exit>` (master AND own) read as an OR-bypass
#   B4 a std::thread(fn) launched inside if (forcingAllowed()) did not gate fn
#   B5 a forward declaration above a function became that function's name
#   B6 lambdas passed to registerExport("X", ...) were not recognised as exports
#   B7 helpers called only from exports were not treated as export code
# Each fix is a switch ONLY so scripts/test_audit_forcing.py --mutate can revert
# it and prove its fixture (scripts/tests/audit_forcing/) catches the bug. The
# audit always runs with every fix on; nothing else may flip these.
FIX = {"B1": True, "B2": True, "B3": True, "B4": True, "B5": True, "B6": True, "B7": True}
# iter-463 round 2 (independent verification of B1..B7 found these, all able to
# misclassify in the UNSAFE direction - a write that runs in the default exe
# reported as MASTER / ALLOW / opt-in):
#   D1 an early-exit guard counted when its block merely ENDED in a return
#      (`if (!M || !own) { if (va == 0) return; }` does not always exit)
#   D2 gates were judged per FUNCTION (master anywhere in the body won, a guard
#      placed after the write counted); now per WRITE SITE (SiteGates)
#   D3 the export rule ran before the gate check (a master-gated fabrication in
#      an export read ALLOW) and covered threads the export spawns
#   D4 using-declarations (`using sr::diag::hackOn;`) were not resolved
#   D5 several own gates on one site: the first in the text won, not the weakest
#   D6 the fallback returned OWNGATE (opt-in) for the first getenv in the body,
#      whatever its default and whether or not it guarded the site; predicate
#      defaults were read from a fixed window that spilled into the next function
#   D7 blind spots: hand-written guest/*.cpp was not scanned; stores through a
#      guestVAToHost / guest-base pointer and memcpy into guest memory were not
#      primitives
#   D9 dead-code fallback: a function referenced only at file scope (a callback
#      table, a static initializer) or a guest-called override (sub_XXXXXXXX,
#      called from generated code) read "dead code" (OWNGATE)
#   D10 (found while fixing D2/D6) a caller's KILL SWITCH - `if (testMode)
#      return; runGuestHarness();` in SDL_main - read as a default-ON gate on
#      everything below it, so no write reached from main could be UNGATED
# Same contract as B1..B7: a switch ONLY for the mutation test.
FIX.update({"D1": True, "D2": True, "D3": True, "D4": True, "D5": True, "D6": True,
            "D7": True, "D9": True, "D10": True})
# iter-463 round 4 (the round-3 verifier's nine remaining defects; each fix is a
# switch ONLY for scripts/test_audit_forcing.py, fixture dNN_*):
#   D11 the guest-pointer model (GuestWrites): every spelling of the guest base
#       (base[x], getGuestBase()/waitGuestBase()/getGlobalGuestMemory out-params,
#       g_guestBaseTls, wrapper returns like lockWord, fed pointer parameters,
#       late-assigned pointers, references) and every write shape through it
#   D12 every write occurrence on a line is its own site at its own position
#       (and every caller reference on a line is judged on its own)
#   D13 a bool flag set from ANOTHER file or through a by-reference argument is
#       not its initializer's constant
#   D14 an OR-bypass of the master gate is judged before the export rule and the
#       allowlist; a helper an export calls only under a gate is not export code
#   D15 a `*ForTest` hook setter called from production code is not trusted
#   D16 hand-written guest code (tools/regen/hand/ppc_missing_recomp_stubs.cpp,
#       the PPC_CALL_INDIRECT_FUNC fake returns in tools/regen/hand/ppc_context.h)
#       is classified; register fake-returns in hand guest bodies are sites
#   D17 a function referenced only from generated / hand-written guest code or a
#       header is not "dead code"
#   D18 generated-code hand edits: gates judged with SiteGates (real default,
#       dominance), and unqualified host calls are hand edits too
FIX.update({"D11": True, "D12": True, "D13": True, "D14": True, "D15": True, "D16": True,
            "D17": True, "D18": True})
# iter-463 burndown round 2 (the round-4 verifier's U1):
#   D20 a FAKE RETURN in a kernel / hand-written guest/*.cpp strong-def override
#       of a guest function (`void sub_XXXXXXXX(PPCContext&..)` / PPC_FUNC(sub_X))
#       was not a site at all: `if (bad) { ctx.r3.u32 = 0; return; }` skips the
#       guest body and fabricates its result, but only the D16 primitive over
#       tools/regen/hand saw register writes. Now every guest-register write in
#       such a body that is NOT followed by a call of the guest body
#       (`__imp__sub_X(`) before the next `return` / the end of the function is
#       a `fake_return` site, judged like every other site (16 such branches ran
#       with no gate or default ON when this landed).
#   D21 (found while landing D20) an override of a guest function that ALSO has
#       a gated host reference (gpu_ring_alloc_fix's master-gated probe takes
#       &sub_82467F40) read "caller-gated: MASTER": the weakest-caller walk
#       counted only host references, and the B4 lift then raised it, although
#       generated guest code calls the override on every allocation. A guest-
#       called override now always carries an UNGATED guest caller.
FIX.update({"D20": True, "D21": True})
# iter-463 phase 2 (docs/DEVTOOLS_DESIGN.md 2.1, A1): the DEV-TOOL class.
#   D40 devToolAllowed("<tool>") / devToolHookNeeded_sub_X() are recognized AT THE SITE as
#       a gate of their own level, DEVTOOL (a declared debugging aid, default OFF, separate
#       from forcing). Without it every dev-tool site reads UNGATED. DEVTOOL ranks with
#       MASTER for pass/fail but prints in its own bucket, keyed by tool; the hard rules
#       (choke point, closed set == docs/DEVTOOLS.json, literal tool names, no OR, no
#       default-ON, observe tools never write, no mutual reference with forcingAllowed) are
#       devtool_report() below. Same contract as B1..D21: a switch ONLY for the mutation test.
FIX.update({"D40": True})
# iter-467 (found while speeding the audit up):
#   D22 func_name took the FIRST `word(` of a signature as the function's name when
#       the main pattern failed (a signature ending in `{`), so a leading attribute
#       named the function after it: `__attribute__((preserve_most, noinline))
#       uint32_t aliasGuestVASlow(..)` was "__attribute__" (also __declspec, alignas).
#       "__attribute__" then joined the host-function set, and every generated
#       `__attribute__((alias("__imp__sub_X"))) PPC_WEAK_FUNC(sub_X);` line read as
#       an unqualified host call: 34,513 bogus generated-code "hand edits" (one per
#       function), each judged by generated_level - the bulk of the audit's run
#       time, and noise that buried the real list. Attribute specifiers are now
#       removed before naming. Same contract as B1..D40: a switch ONLY for the
#       mutation test.
FIX.update({"D22": True})
_ATTR_SPEC_RE = re.compile(
    r"__attribute__\s*\(\((?:[^()]|\((?:[^()]|\([^()]*\))*\))*\)\)"
    r"|(?:__declspec|\balignas)\s*\((?:[^()]|\([^()]*\))*\)")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audit_host_threads as _aht  # noqa: E402  (strip_code, Src, gate_of)

_OR_RHS_RE = re.compile(r"\|\|\s*(?:sr::kernel::)?forcingAllowed\s*\(")
_OR_LHS_RE = re.compile(r"(!\s*)?(?:sr::kernel::)?forcingAllowed\s*\(\s*\)\s*\|\|")


def _exit_guard(clean, pos):
    """B3: is the `!forcingAllowed() ||` at `pos` the whole-condition operand of an
    `if (...)` whose branch only EXITS (return / continue / break / goto, or a
    block ending in one)? Then the code after it runs only with the master gate
    ON *and* the other operand false: master AND own, not a bypass."""
    for m in reversed(list(re.finditer(r"\bif\s*\(", clean[:pos]))):
        op = m.end() - 1
        depth, close = 0, None
        for i in range(op, len(clean)):
            if clean[i] == "(":
                depth += 1
            elif clean[i] == ")":
                depth -= 1
                if depth == 0:
                    close = i
                    break
        if close is None or close < pos:
            continue
        cond = clean[op + 1:close]
        rel = pos - (op + 1)
        # top level of the condition, no && anywhere (a && (!M || x) can run without M)
        if "&&" in cond or cond[:rel].count("(") != cond[:rel].count(")"):
            return False
        after = clean[close + 1:]
        if re.match(r"\s*(?:return|continue|break|goto)\b", after):
            return True
        b = re.match(r"\s*\{", after)
        if not b:
            return False
        ob = close + 1 + b.end() - 1
        depth = 0
        for i in range(ob, len(clean)):
            if clean[i] == "{":
                depth += 1
            elif clean[i] == "}":
                depth -= 1
                if depth == 0:
                    inner = clean[ob + 1:i]
                    return bool(re.search(r"\b(?:return|continue|break|goto)\b[^;{}]*;\s*$", inner))
        return False
    return False


def or_bypass(text):
    """True when text ORs the master gate with another operand in a way that can
    run the guarded code with SR_ALLOW_FORCING off."""
    if not FIX["B3"]:
        return bool(OR_BYPASS_RE.search(text))
    clean = _aht.strip_code(text)
    if _OR_RHS_RE.search(clean):
        return True
    for m in _OR_LHS_RE.finditer(clean):
        if not m.group(1) or not _exit_guard(clean, m.start()):
            return True
    return False
# iter-463: DECLARED band-aids that ship ungated. The parser fix surfaced these;
# they are NOT platform fixes, so they are never ALLOW. Each names its row in
# docs/BANDAIDS.md (the row must exist or the audit fails) and is reported in
# the DEFAULT-ON (forcing that ships) section until it is gated or deleted.
# (file_substr, enclosing function, BANDAIDS.md row title substring)
DECLARED_BANDAIDS = [
    # iter-463 Group A: advanceFencesIfStalled ("GPU interrupt fence advance")
    # left this list - its only launch, startGpuInterruptThread, is master-gated
    # (early return + the forcingAllowed() block in the legacy VdSwap path), so
    # caller propagation now classifies it MASTER.
]


def _bandaids_text():
    try:
        return open(os.path.join(ROOT, "docs", "BANDAIDS.md"), encoding="utf-8").read()
    except OSError:
        return ""

# Explicit, REVIEWED platform-fix allowlist (implements a console facility the PC
# lacks; the guest still makes every decision). Match = (file_substr, code_anchor)
# where code_anchor is a substring on/near the site's line or its function. Keep
# this SMALL and JUSTIFIED -- over-allowlisting is how forcing hides. Reviewable.
ALLOWLIST = [
    # tools/graceful-quit (docs/TOOLKIT.md 1.8m): the quit channel's ACK is a HOST named event the game creates for the
    # harness (Local\SR_QUIT_ACK_<id>_<pid>, automated runs only); no guest object, no guest code reads it. (The audit
    # reaches it through the name watcherMain, which other files also define.)
    ("kernel/quit.cpp", "fn:pAck",
     "tools/graceful-quit: SetEvent on the harness's ack event Local\\SR_QUIT_ACK_<id>_<pid> - a host-only named "
     "event, never guest-visible; SR_INSTANCE_ID runs only"),
    # iter-467 native-a boot.device_create (docs/NATIVE_ROADMAP.md "boot.device_create BUILD PLAN"; OVERRIDES.json
    # sub_820FDE90 / sub_82110EB8 / sub_8210E520 / sub_820F0048, class platform-fix, gate SR_NATIVE_DEVCREATE on
    # the native transport only): the device-create ports' device-field stores are the XDK's own, with the asm's
    # values (D3DDeviceInitRingBuffer's transport fields, GpuCreateBlockInterruptThreads' per-CPU blocks,
    # D3DDeviceResetState's state words, the segment re-emit's dirty masks) - console parity, not forcing.
    ("kernel/native_devcreate.cpp", "fn:wr",
     "boot.device_create: D3DDeviceInitRingBuffer_820FDE90 / GpuCreateBlockInterruptThreads_82110EB8 ported from the asm: "
     "the device fields the XDK stores (platform-fix, gate SR_NATIVE_DEVCREATE)"),
    ("kernel/native_devcreate.cpp", "fn:wr8",
     "boot.device_create: the same ports' byte stores (the list-head / block flags the XDK clears)"),
    ("kernel/native_flush.cpp", "fn:devResetLeaf",
     "boot.device_create: D3DDeviceResetState_8210E520's own device writes (devResetFields, the asm's values) before its "
     "packet tail (platform-fix, gate SR_NATIVE_DEVCREATE)"),
    ("kernel/native_flush.cpp", "fn:segmentBeginLeaf",
     "boot.device_create: the segment re-emit 820F0048's dirty masks dev+0..+39 = ~0 after the tile rects, as the asm "
     "(platform-fix, gate SR_NATIVE_DEVCREATE)"),
    # M2 display.source0 (2026-10-13): the native port of the XDK D3D ISR's source-0 (VSYNC) branch,
    # IDA 820FDC30-820FDCA8 inside sub_820FDBA0 - frame counter +1, positive countdown -1, on 1->0 the
    # scratch+4 clear and last-release reload, then the registered callback with the SDK's {frame, ctx, 0}
    # block. XDK library code the console ran, not a game decision; differential-tested against
    # __imp__sub_820FDBA0 (difftest --native-display-test). Gate SR_NATIVE_DISPLAY_SOURCE0=1, default OFF.
    ("kernel/native_display.cpp", "fn:trySource0",
     "display.source0: the XDK D3D vblank ISR's own device / stack stores (sub_820FDBA0 source 0, the asm's "
     "order and values) ported native (platform-fix, gate SR_NATIVE_DISPLAY_SOURCE0, default OFF)"),
    # display.registers (2026-10-14, docs/NATIVE_ROADMAP.md 'DISPLAY.REGISTERS BUILD NOTE'): the XDK pacing callback
    # GpuPresentPacingUpdate_82111E38 (IDA 82111E38-82111EC4) as host code - the same stores in the asm's order and
    # values (+0x3B20, +0x3B1C, pc+4 = 0, +0x3B18 = +0x3B14) with line % from today's scan-out answers held by a host
    # counter instead of the emulated registers. XDK library code, not a game decision; differential-tested against
    # __imp__sub_82111E38 (difftest --native-vblank-test). Gate SR_NATIVE_VBLANK, default ON since the display-registers
    # bake (native/dreg-bake, 2026-10-15); =0 keeps the guest body.
    ("kernel/native_vblank.cpp", "fn:pacingCore",
     "display.registers: the XDK pacing callback's own device / scratch stores (sub_82111E38, the asm's order and "
     "values) ported native on today's scan-out answers held by host code (native, gate SR_NATIVE_VBLANK, default "
     "ON since the display-registers bake; =0 the guest body)"),
    ("gfx/",                    "",             "renderer PM4->D3D12: GPU emulation (charter-allowed)"),
    ("recomp_switch_fixes",     "",             "XenonRecomp dropped switch/bctr case bodies: recompiler-hole reimpl"),
    ("guest_context.cpp",       "kEDRAMBase",   "EDRAM console memory the PC lacks: zero-init"),
    ("guest_context.cpp",       "kGPURegBase",  "GPU register region: zero-init"),
    ("guest_context.cpp",       "KeTimeStamp",  "KeTimeStampBundle: real XEX kernel DATA import"),
    ("guest_context.cpp",       "kFallbackCritSect", "critical-section object the PC must provide"),
    ("audio_render_driver.cpp", "kStackSize",   "guest stack for the XAudio render thread the console's audio subsystem provides"),
    ("kernel/anim_heap.cpp",    "line:sr::kernel::writeGuestBE32(kHeapAnimationSizeVA, want);", "iter-465 charswitch PORT FEATURE (OVERRIDES.json sub_82186500, class port-feature): the HeapAnimation size dword of the boot heap table, raised before MemoryPoolSystemInit runs only when own-project characters are preloaded (SR_CHARACTER_SWITCH, unset = the port-feature switch, so natural rolls never write) or SR_ANIM_HEAP_MB is set; a memory budget, not a game decision (the game allocates and frees in it itself)"),
    ("kernel/apt_call.cpp",     "fn:invoke", "iter-466 controls2 PORT FEATURE (USER DECISION 2026-10-06, charter 2a; OVERRIDES.json sub_826D2718 port-feature): the game's own AS invoke sequence (AsInvokeByPath_sub_826B8DC0 + AptAS2InvokeByPathCtxRaw_sub_826BB990 transcribed) for the player's mouse - a menu's own focus / value method or gEH.HandleEvent, one per explicit mouse input; the host frame below r1, the published Apt context and the AS stack are the game's own protocol, restored afterwards. Refuses unless automation::portFeaturesOn() (natural rolls never call)"),
    ("kernel/r1_platform.cpp", "fn:sub_82762400", "R1 PLATFORM (iter-350 platform fix, carried by a splice body until R1, docs/SPLICE_CONVERSION.md): Lua luaD_rawrunprotected on host setjmp/longjmp - a guest longjmp must unwind HOST frames, which recompiled code cannot express; the frame, the lua_longjmp struct (r1+80, status at r1+416) and the L->errorJmp chain (L+80) are the game's own protocol"),
    ("kernel/r1_platform.cpp", "fn:sub_827A45E0", "R1 PLATFORM (iter-467 option A, _NLG parity; OVERRIDES.json sub_827A45E0 platform-fix): Crt_Setjmp's host half writes the jmp_buf LR slot (+0x134). The console's setjmp fills it with the return address of its `bl`; the generated body cannot, because the generated `bl` does not set LR and `mflr` emits nothing. The value is the console's: the call site + 4 of a protected setjmp user, or luaD_rawrunprotected's 0x82762440. Only Crt_Longjmp reads it, passing it to _NLG_Notify"),
    ("kernel/mod_bridge.cpp",   "line:writeGuestBE32(frame, savedR1);", "iter-463 SR_GetSettings / SR_SetSettings reply (mod API return channel, SR_* = mod content only): the host's own guest stack frame below r1 (back chain + target path + method name) for the AptExecutor invoke of FlowMgr.SR_OnHostSettings the mod asked for; OVERRIDES.json sub_826D39A0 lists the guest calls"),
    ("audio_render_driver.cpp", "writeGuestBE32_local(r4", "XAudioRegisterRenderDriverClient: handle out-param (import impl)"),
    # iter-463 manual review of the 55 DEFAULT-ON sites: the platform code that is
    # not an export (so B6/B7 cannot see it) but implements a console facility.
    # "fn:NAME" anchors match the enclosing function by name.
    ("kernel/kthread_objects.cpp", "fn:kthreadInitBlock",
     "KTHREAD object init (SR_OB_KTHREAD) = Xenia XThread::InitializeGuestObject "
     "(xthread.cc:180-250; 0xAB0 block, xthread.h:372)"),
    ("kernel/kthread_objects.cpp", "fn:kthreadMarkExitedVA",
     "KTHREAD exit_status / terminated / signal_state on thread exit = Xenia XThread::Exit "
     "(xthread.cc:482-489)"),
    # iter-466 stutter lane: SR_ASYNC_READ (default ON) - the kernel's own completion of an
    # overlapped read, moved off the calling thread (docs/PERFORMANCE.md 7.3).
    ("kernel/stubs.cpp", "fn:aioComplete",
     "SR_ASYNC_READ: an overlapped NtReadFile completes on the async worker: it fills the "
     "caller's buffer + IOSB and signals the caller's event / the file object / the issuer's "
     "alertable wait (NT I/O completion; the console completes overlapped reads asynchronously)"),
    ("kernel/stubs.cpp", "fn:stub_NetDll_XNetGetTitleXnAddr",
     "RELEASE_PLAN phase 4 row 2 (docs/FORCING_RETIREMENT.md): NetDll_XNetGetTitleXnAddr fills the "
     "caller's XNADDR out-param and answers XNET_GET_XNADDR_STATIC (4) as Xenia's xam_net.cc does "
     "(import impl). A reasoned choice, not a copy: the game's 'conn' poll in sub_8229F460 leaves "
     "'~ini' on any terminal state - a console with no cable reports NONE (1), one with an address "
     "STATIC (4) - and never on PENDING (0), which the generic stub answered forever; STATIC + "
     "Xenia's loopback XNADDR keeps the networking state machine on the path every oracle roll and "
     "golden dump took, where NONE would steer the title down a no-network branch no oracle run has "
     "exercised (docs/OVERRIDES.json sub_8229F460 evidence)"),
    ("kernel/stubs.cpp", "fn:aioSubmit",
     "SR_ASYNC_READ: queues an overlapped read to the async read worker (no guest state)"),
    ("kernel/stubs.cpp", "fn:signalThreadCompletion",
     "an exiting guest thread signals its thread object (Xenia xthread.h GetWaitHandle); "
     "called only from ExTerminateThread and the guest thread's own return"),
    # step4/audit batch 2 (Astra P2, SR_XNOTIFY_XENIA, baked default ON 2026-10-20): a notification listener's wait
    # handle.
    ("kernel/stubs.cpp", "fn:xnEnqueueTo",
     "SR_XNOTIFY_XENIA: enqueueing a notification sets the listener's own manual-reset wait event, as Xenia's "
     "XNotifyListener::EnqueueNotification sets wait_handle_ (xnotifylistener.cc:50); the event exists only for "
     "Xenia-path listeners (null otherwise), the notification itself is the platform's (guest call or our XAM UI "
     "model); the boot-delivery thread enqueues only with the gate off (no event) or under the master-gated "
     "SR_XNOTIFY_FORCE_BOOTUI"),
    # iter-463 round 2 (D7 scans hand-written guest/*.cpp): exact-code anchors.
    ("guest/guest_harness.cpp", "std::memset(base, 0, allocSize);",
     "the harness zero-fills the guest address space it just reserved, before the "
     "XEX loads: fresh console memory reads as zero"),
    ("guest/guest_harness.cpp", "std::memset(base + kMainStackBase, 0, kMainStackSize);",
     "the main thread's stack, zeroed before the entry point runs (the kernel hands a "
     "new thread a fresh stack)"),
    ("kernel/gpu_submit.cpp", '"GPU_CP"',
     "GPU command processor dispatching the guest's registered graphics ISR on PM4 "
     "INTERRUPT = Xenia pm4_command_processor_implement.h:630-641, graphics_system.cc:331-334"),
    # iter-463 round 3: classification of the 56 sites that run in the default
    # exe (31 DEFAULT-ON + 25 UNGATED after the round-2 site-level audit). Only
    # the PLATFORM ones are listed here, each with its Xenia equivalent
    # (reference/xenia-canary-oracle/src/xenia). FORCING and DORMANT sites stay
    # flagged until a kernel-owning change gates or deletes them. Anchors are
    # exact code on the site line (the +-5-line window was checked so that no
    # FORCING/DORMANT neighbour is covered; test_audit_forcing.py pins that).
    ("kernel/guest_context.cpp", "kVdGlobalVarVA",
     "VdGlobalDevice DATA import (SR_VD_GLOBAL_DEVICE): cell 0x82000948 -> a zeroed kernel "
     "variable the title stores its device into = Xenia xboxkrnl_video.cc:564-567 (0x801E6FC4)"),
    ("kernel/guest_context.cpp", "g_guestBase + s.",
     "the remaining xboxkrnl DATA imports (SR_DATA_IMPORTS): cell -> kernel variable = Xenia "
     "export_resolver SetVariableMapping (xboxkrnl_module.cc:92-220, xboxkrnl_video.cc:579-590, "
     "kernel_state.cc:1576-1666 ExThreadObjectType)"),
    ("kernel/guest_context.cpp", "std::memcpy(g_guestBase + 0xD001",
     "DATA import variable contents: ExLoadedCommandLine \"default.xex\" (xboxkrnl_module.cc:"
     "185-200), VdGpuClockInMHz (xboxkrnl_video.cc:582), XboxKrnlVersion 2.0.1888.0 "
     "(xboxkrnl_module.cc:205-209), VdHSIOCalibrationLock critical-section init "
     "(xboxkrnl_video.cc:585-591). iter-463: VdGpuClockInMHz fixed to 00 00 01 F4 (500)"),
    ("kernel/guest_context.cpp", "writeBE64(bundle",
     "KeTimeStampBundle interrupt/system time (SR_KE_TIMESTAMP) = Xenia kernel_state.cc:"
     "1274-1281 (X_TIME_STAMP_BUNDLE updated by the kernel timer)"),
    ("kernel/guest_context.cpp", "writeGuestBE32(bundle + 0x10",
     "KeTimeStampBundle tick_count (uptime ms) = Xenia kernel_state.cc:1281"),
    # iter-463 timebase: the SR_TIMEBASE_50MHZ (default ON) ticker writes the same three
    # bundle fields from one 50 MHz tick read (kernel/sr_timebase.cpp writeBundle).
    ("kernel/guest_context.cpp", "sr::timebase::writeBundle(bundle",
     "KeTimeStampBundle interrupt/system time + tick_count from the 50 MHz guest clock "
     "(SR_KE_TIMESTAMP / SR_TIMEBASE_50MHZ) = Xenia kernel_state.cc:1274-1281 "
     "(X_TIME_STAMP_BUNDLE updated by the kernel timer from Clock::QueryGuestUptimeMillis "
     "/ QueryGuestInterruptTime / QueryGuestSystemTime)"),
    ("kernel/guest_context.cpp", "g_guestBase + kBundleCell",
     "KeTimeStampBundle DATA import cell -> bundle = Xenia kernel_state.cc:1309-1328"),
    # iter-463 burndown item 6: the XexExecutableModuleHandle block moved out of
    # .text (0x82100000..0x821002FF) into the kernel-variable region, and the
    # hmodule carries Xenia's LDR-entry fields; exact line anchors.
    ("kernel/guest_context.cpp", "line:writeGuestBE32(g_guestBase + kXexModuleCell, kXexModuleVarVA);",
     "XexExecutableModuleHandle DATA import: cell 0x82000768 (raw token 0x93010100) -> kernel "
     "variable 0xD001C000 = Xenia xboxkrnl_module.cc:161-167 (variable 0x802080D4)"),
    ("kernel/guest_context.cpp", "line:writeGuestBE32(g_guestBase + kXexModuleVarVA, kXexHModuleVA);",
     "*variable = hmodule_ptr = Xenia kernel_state.cc:489-497"),
    ("kernel/guest_context.cpp", "line:writeGuestBE32(hmod + 0x",
     "hmodule = X_LDR_DATA_TABLE_ENTRY fields (image_base, full_image_size, entry_point, "
     "time_date_stamp, xex_header_base) = Xenia xmodule.h:27-53, values from golden sound_menu "
     "0x30013000"),
    ("kernel/guest_context.cpp", "line:std::memset(g_guestBase + kXexModuleVarVA, 0, 0x200);",
     "zeroed kernel variable + hmodule page before its fields are set (fresh kernel memory)"),
    ("kernel/guest_context.cpp", "line:std::memset(g_guestBase + kXexHeaderStubVA, 0, 0x100);",
     "zeroed xex-header block the hmodule's xex_header_base points at (RtlImageXexHeaderField's "
     "legacy SR_XEX_HEAP_FIELD=0 arm returns it)"),
    ("kernel/guest_context.cpp", "fn:guestHeapAlloc",
     "host kernel pool (NtAVM / physical / XMA context / thread stacks): fresh grants read "
     "zero = Xenia Memory::SystemHeapAlloc Zero(address, size) (memory.cc:715-724), XamAllocImpl "
     "(xam_info.cc:536-552) and NtAllocateVirtualMemory first commit (xboxkrnl_memory.cc:186-199); "
     "the bump never reuses a VA, so on a correct grant the fill is a no-op. The in-code comment "
     "('bound runaway scans') is stale rationale, not the contract"),
    ("kernel/guest_context.cpp", "fn:guestHeapAllocFallback",
     "fallback kernel pool, same contract = Xenia Memory::SystemHeapAlloc (memory.cc:715-724)"),
    ("kernel/gpu_submit.cpp", "fn:writeBackRptr",
     "CP read-pointer writeback to the guest's CP_RB_RPTR_ADDR = Xenia command_processor.cc:"
     "393-395"),
    ("kernel/gpu_ringless.cpp", "fn:updateWritePointer",
     "Native port of retail 820FD218 (IDA contract in gpu_ringless.cpp): calls the real "
     "820FD150 ring-space wait with dev/wptr/3, publishes the native segment in that index "
     "space, advances dev+0x2A44 by three modulo its mask and returns the SDK's zero. "
     "Only the guest's own publication invokes it; invalid native admission is fatal. "
     "Stage 2b makes this platform operation unconditional in release, with no guest-ring fallback."),
    ("kernel/gpu_ringless.cpp", "fn:sub_82110140",
     "Native port of retail 82110140 (IDA contract in gpu_ringless.cpp): derives the "
     "bin-select value from the guest's S flags, updates S+0x16C only when changed, calls "
     "the real 820FD150 space wait with dev/wptr/2, publishes that value and advances "
     "dev+0x2A44 by two. Its SDK-call/return registers and quit-cut return are the existing "
     "native producer ABI, not synthesized input or forced game progress."),
    ("kernel/gpu_submit.cpp", "c.pcrVA + 268u",
     "SR_KPCR-off arm of the DPC's active-CPU byte (KPCR+268) = Xenia "
     "EmulateCPInterruptDPC -> SetActiveCpu (kernel_state.cc:1398-1426)"),
    ("kernel/gpu_submit.cpp", "std::memset(p, 0, 0x1000)",
     "SR_KPCR-off arm: the CP thread's own zeroed PCR page (Xenia gives every XThread a PCR, "
     "xthread.cc:399-409)"),
    ("kernel/kpcr.cpp", "fn:initPcr",
     "per-thread KPCR (SR_KPCR) = Xenia XThread::Create PCR init (xthread.cc:399-409)"),
    # (round 4, verifier item 9: `fn:writeU8` allowlisted a GENERIC guest-byte
    # writer, so any new writeU8 caller was covered. writeU8's site now takes its
    # callers' class; the platform callers are listed by name.)
    ("kernel/kpcr.cpp", "fn:kpcrSetActiveCpu",
     "KPCR/KTHREAD current-cpu bytes = Xenia XThread::SetActiveCpu (xthread.cc:462)"),
    ("kernel/kpcr.cpp", "fn:kpcrBindCurrent",
     "KPCR stack base/limit once the runner knows the real stack = Xenia xthread.cc:399-409"),
    ("kernel/kpcr.cpp", "fn:KpcrDpcScope",
     "DPC impersonation dpc_active flag = Xenia KernelState::BeginDPCImpersonation / "
     "EndDPCImpersonation (kernel_state.cc:1382-1396)"),
    # iter-463 audio review: the three DEFAULT-ON sites the audio flip (391f9d6) added are
    # kernel facilities, not game state (the audit read 0 DEFAULT-ON before the flip, 4 after).
    ("kernel/kpcr.cpp", "fn:kthreadSetProcessType",
     "KTHREAD process_type / process_type_dup (SR_PROCTYPE) = Xenia "
     "XThread::InitializeGuestObject copies the owning process's type into every KTHREAD "
     "(xthread.cc:215-217): TITLE for title threads, SYSTEM for the audio worker"),
    ("kernel/apu/xma_device.cpp", "fn:ensureInit",
     "XMA context array (SR_XMA_NEW): the kernel's own 320 x 64-byte physical allocation, "
     "zeroed = Xenia XmaDecoder::Setup SystemHeapAlloc(physical) (xma_decoder.cc:144-149)"),
    ("kernel/stubs.cpp", "fn:keWmUndoHost",
     "KeWaitForMultipleObjects WaitAll (SR_KE_WAITMULTI): gives back a host acquisition "
     "(semaphore count / event token) the attempt took when a guest-state CAS lost a race, so "
     "the wait stays all-or-nothing = Xenia XObject::WaitMultiple (xobject.cc:265-340); "
     "objects end exactly as they were before the attempt"),
    # iter-463 round 4 (verifier item 9): the round-2 anchors `// back chain`
    # (a COMMENT) and `0xD000` (a substring of every kernel-variable VA) are gone;
    # the mod-bridge reply is listed by exact code on each site line.
    ("kernel/mod_bridge.cpp", 'line:std::memcpy(guestVAToHost(nameVA), r.method.c_str(), nameLen + 1);',
     "mod-bridge reply (SR_GetSettings / SR_SetSettings, iter-464 SR_GetModOptions / "
     "SR_SetModOption / SR_SaveEvent query / SR_DevMenu): the method-name string in the "
     "host's own guest frame (see the back-chain entry above)"),
    ("kernel/mod_bridge.cpp", 'line:std::memcpy(guestVAToHost(strVA), r.args[i].s.c_str(), r.args[i].s.size() + 1);',
     "iter-464 mod-bridge reply with a STRING argument (SR_DevMenu page titles / row labels): "
     "the text is copied into the same host-owned guest frame below r1 (frame+0x100.., "
     "<= 0x700 bytes, under the 0x900 the frame reserves) and handed to "
     "AptObjectRecycleOrAlloc_sub_8269A210, which copies it into an Apt string - the call "
     "FlowMgr_CppShowPopup_sub_822B15F8 makes for popup titles. Reached only by mod content "
     "(SR_* fscommands); OVERRIDES.json sub_826D39A0 lists the guest calls"),
    ("kernel/mod_bridge.cpp", 'line:std::memcpy(guestVAToHost(pathVA), "FlowMgr", 8);',
     "SR_GetSettings / SR_SetSettings reply: the target path string in the host's own guest "
     "frame (see the back-chain entry above)"),
    # iter-463 burndown (D20 made kernel fake returns sites): the PLATFORM ones.
    ("kernel/apt_parse_trace.cpp", "line:ctx.r3.u32 = (uint32_t)ms;",
     "sub_82564E80 = the engine's tick source (*DAT_8297932c)(): it returns elapsed "
     "milliseconds (sub_825667C8 = rdtsc/(freq/1000), sub_82566818 = KeTimeStampBundle "
     "TickCount ms). Host monotonic ms is the value the console clock produces - the guest "
     "asks for the time, we provide it (Xenia: the guest's own path over a real timebase "
     "and KeTimeStampBundle, kernel_state.cc:1274-1281). Retire when the guest's own "
     "timebase path (mftb / KeQueryPerformanceCounter) is proven equal"),
    ("kernel/mod_bridge.cpp", "line:ctx.r3.u64 = 0;",
     "sub_826D39A0 (Apt FSCommand handler) answers ONLY SR_-prefixed mod-bridge commands "
     "(docs/MODS.md); retail content sends 0 FSCommands and every non-SR_ command runs "
     "the game's own body (the branch above). Mod API return value, not a guest result"),
    ("kernel/gpu_ring_alloc_fix.cpp", "line:ctx.r3.u32 = 0x10u;",
     "SR_MALLOC0_FIX: an ARGUMENT rewrite that feeds the guest body (bit-31-clear "
     "malloc(0) -> 0x10, then __imp__sub_82467F40 runs; D20's textual scan sees the "
     "bit-31 branch's return first). Registry platform-fix: console dlmalloc returns a "
     "valid block for malloc(0) (iter-381 A/B). OPEN: prove against Xenia's "
     "xeMmAllocatePhysicalMemoryEx / the guest's own size-0 path, then move the fix there"),
    # iter-463 round 4: newly visible default-running PLATFORM sites (D11 / D16).
    ("guest/guest_harness.cpp", "line:sr::kernel::loadXexImage(xexPath, base,",
     "the loader copies the XEX image into guest memory at its load address before the "
     "entry point runs = Xenia XexModule::ReadImageUncompressed (cpu/xex_module.cc:567-596: "
     "memset the image range, memcpy the image)"),
    ("tools/regen/hand/ppc_missing_recomp_stubs.cpp", "line:PPC_STORE_U32(ctx.r11.u32, ctx.r10.u32);",
     "sub_827A0000 = the guest's own `stw r10,0(r11)` (IDA bytes 91 4B 00 00 at 0x827A0000; "
     "the generated weak body in ppc_recomp.112.cpp is the same store). The hand stub adds a "
     "null guard and duplicates the generated body: redundant, see the kernel-change list"),
    ("kernel/stubs.cpp", "fn:vdVblankThread",
     "60 Hz vblank firing the ISR the title registered (VdSetGraphicsInterruptCallback) = Xenia "
     "GraphicsSystem vsync worker MarkVblank -> DispatchInterruptCallback(0, 2) "
     "(graphics_system.cc:118-172, 331-345)"),
    # iter-467 p2 divergence (2): the display controller's scan-out registers the guest reads in
    # GpuPresentPacingUpdate_sub_82111E38 (SR_VD_SCANOUT_REGS, baked default ON).
    ("kernel/stubs.cpp", "fn:vdScanoutRegsTick",
     "display-controller registers D1MODE_V_COUNTER (0x194C) / D1MODE_VIEWPORT_SIZE (0x1961) as Xenia "
     "answers them: GraphicsSystem::ReadRegister 0x1961 -> 0x050002D0 and the frame limiter's V_COUNTER "
     "advance (graphics_system.cc:143-144, 273-295); run by the vblank thread before the source-0 ISR. "
     "Emulation-path platform (the native P2-B GATE item replaces the guest's reads)"),
]


def strip_for_braces(line, in_block):
    """Remove comments/strings so brace counting is reliable. Returns (clean, in_block)."""
    out, i, n = [], 0, len(line)
    while i < n:
        two = line[i:i+2]
        if in_block:
            if two == "*/":
                in_block = False; i += 2; continue
            i += 1; continue
        if two == "/*":
            in_block = True; i += 2; continue
        if two == "//":
            break
        c = line[i]
        if c == '"' or c == "'":
            q = c; i += 1
            while i < n:
                if line[i] == "\\": i += 2; continue
                if line[i] == q: i += 1; break
                i += 1
            continue
        out.append(c); i += 1
    return "".join(out), in_block


_NS_OPEN_RE = re.compile(r"(?:\bnamespace\b[\s\w:]*|\bextern)\s*$")


def parse_functions(lines):
    """Return list of (start_idx, end_idx, signature) top-level function blocks.

    iter-463: `namespace X {` / `extern "C" {` blocks are SCOPES, not functions.
    The old parser counted raw brace depth, so a file wrapped in
    `namespace sr { namespace kernel {` (kernel/stubs.cpp) parsed as ONE giant
    function: every site in it shared one body, and one forcingAllowed() anywhere
    in the file classified all 81 of its writes MASTER.

    B5: the signature is the code since the last scope-level `;` / `}`, comments
    and preprocessor lines dropped. The raw 6-line window made the forward
    declaration `void notifyFMWorkerCreated(...);` above stub_ExCreateThread the
    name of stub_ExCreateThread, so the export check missed it."""
    funcs, in_block = [], False
    stack = []                    # "ns" | "blk" per open brace
    fn_start, fn_sig, pending = None, None, []
    since = ""                    # clean text since the last ; { } at scope level

    def at_scope():
        return "blk" not in stack

    for idx, raw in enumerate(lines):
        clean, in_block = strip_for_braces(raw, in_block)
        if at_scope() and clean.strip():
            if not FIX["B5"]:
                pending.append((idx, raw.rstrip()))
            elif not clean.lstrip().startswith("#"):
                pending.append((idx, clean.rstrip()))
            if len(pending) > 6:
                pending = pending[-6:]
        for ci, ch in enumerate(clean):
            if ch == "{":
                if at_scope() and _NS_OPEN_RE.search(since):
                    stack.append("ns")
                    pending, since = [], ""
                    continue
                if at_scope() and FIX["D9"] and (
                        re.search(r"=\s*$", since) or (
                            not re.search(r"[()]", since) and
                            not re.search(r"\b(?:struct|class|union|enum)\b", since))):
                    # D9: `T s_table[] = { fn, .. };` / `std::atomic<T> s_x { .. }`
                    # is an INITIALIZER, not a function: a reference inside it
                    # is a file-scope reference (runs with no gate of its own)
                    stack.append("blk")
                    fn_start, pending, since = None, [], ""
                    continue
                if at_scope():
                    fn_start = idx
                    if FIX["B5"]:
                        # earlier lines + this line UP TO this brace, cut after
                        # the last scope-level ; or } (a prototype, a prior body)
                        sig = " ".join([t.strip() for i, t in pending if i < idx] + [clean[:ci]])
                        # (the brace is kept so func_name() reads it exactly as
                        # before: the first `name(` of the signature)
                        sig = re.split(r"[;}]", sig)[-1].strip() + " {"
                    else:
                        sig = " ".join(t.strip() for _, t in pending).strip()
                    fn_sig = sig[-400:]
                    pending = []
                stack.append("blk")
                since = ""
            elif ch == "}":
                kind = stack.pop() if stack else "blk"
                if kind == "blk" and at_scope() and fn_start is not None:
                    funcs.append((fn_start, idx, fn_sig or "?"))
                    fn_start, fn_sig = None, None
                    pending = []
                since = ""
            elif ch == ";":
                if at_scope():
                    since = ""
            elif at_scope():
                since += ch
        if at_scope():
            since += " "
    return funcs


def build_export_impls(all_files):
    """Names of functions registered as kernel exports (registerExport("X", fn)),
    plus the repo convention stub_<Export> / exp_<Export> (xam_content.cpp
    registers its exp_* table in a loop the literal regex cannot see)."""
    impls = {}
    rx = re.compile(r'registerExport\(\s*"(\w+)"\s*,\s*&?(?:[\w:]*::)?(\w+)\s*\)')
    conv = re.compile(r"^(?:static\s+)?(?:uint32_t|void|int)\s+((?:stub|exp)_(\w+))\s*\(", re.M)
    for lines, _funcs in all_files:
        text = "\n".join(lines)
        for m in rx.finditer(text):
            impls[m.group(2)] = m.group(1)
        for m in conv.finditer(text):
            impls.setdefault(m.group(1), m.group(2))
    return impls


def build_gate_preds(all_files):
    """Global map of gate-predicate function name -> ('MASTER'|env). A gate predicate
    is a bool function whose body reads getenv("SR_..") (an own-gate) OR is
    forcingAllowed()/an observe-only dead-code flag. Folding these in is what stops
    false-positives when the gate lives in a helper, not the write's own function."""
    preds = {"forcingAllowed": ("MASTER", "SR_ALLOW_FORCING")}
    if FIX.get("D40"):
        preds["devToolAllowed"] = ("DEVTOOL", "SR_DEVTOOLS")    # iter-463 phase 2 (A1)
    # Scan raw text (robust to namespace/macro nesting that brace-span parsing
    # misses): a bool function whose head reads getenv("SR_..") is a gate predicate.
    for lines, _funcs in all_files:
        text = "\n".join(lines)
        for m in re.finditer(r"\bbool\s+(\w+)\s*\([^;{)]*\)\s*\{", text):
            g = re.search(r'getenv\(\s*"(SR_[A-Z0-9_]+)"', text[m.end():m.end() + 500])
            if g:
                env = g.group(1)
                # A predicate that reads the MASTER env IS the master gate.
                # iter-402: capture the gate's DEFAULT too. "Gated" is not a
                # single thing: a default-OFF env is the charter's sanctioned
                # opt-in diagnostic, while a default-ON env means the site is
                # ACTIVE IN THE DEFAULT EXE -- forcing that ships, which is the
                # one thing rule 1 forbids. Treating both as "gated" is how
                # SR_AS_AUTO_FRAME fabricated AS register frames by default
                # while this audit reported a clean run.
                w = text[m.end():m.end() + 500]
                default_on = bool(re.search(r"!\s*e\s*\|\|", w)) and not re.search(
                    r"e\s*&&\s*e\[0\]\s*==\s*'1'", w)
                preds[m.group(1)] = (("MASTER", env) if env == "SR_ALLOW_FORCING"
                                     else ("OWNGATE_ON" if default_on else "OWNGATE", env))
        # iter-463: a bool helper that is FALSE unless the master gate is on
        # (`if (!forcingAllowed()) return false;`, `return own && forcingAllowed();`)
        # is a MASTER predicate - rtam_service.cpp gateOn(), stubs.cpp
        # gpuSemAutotrackForcing(). An OR with the master gate is not (OR_BYPASS_RE).
        for m in re.finditer(r"\bbool\s+(\w+)\s*\([^;{)]*\)\s*\{", text):
            if _pred_requires_master(text[m.end():m.end() + 600]):
                preds[m.group(1)] = ("MASTER", "SR_ALLOW_FORCING")
    return preds


def _pred_requires_master(head):
    """A bool helper body that is FALSE unless the master gate is on."""
    end = head.find("\n}")
    head = head[:end] if end >= 0 else head
    if or_bypass(head):
        return False
    return bool(
        re.search(r"if\s*\(\s*!\s*(?:sr::kernel::)?forcingAllowed\s*\(\s*\)\s*\)\s*return\s+false", head)
        or re.search(r"return[^;]*&&\s*(?:sr::kernel::)?forcingAllowed\s*\(", head)
        or re.search(r"return\s+(?:sr::kernel::)?forcingAllowed\s*\(\s*\)\s*&&", head)
        # B3: `if (!forcingAllowed() || !own) return false;` is master AND own
        or (FIX["B3"] and re.search(
            r"if\s*\(\s*!\s*(?:sr::kernel::)?forcingAllowed\s*\(\s*\)\s*\|\|[^;{]*\)\s*return\s+false", head)))


def _ns_spans(clean):
    """[(open, close, [names])] of namespace blocks (anonymous = [""]); extern "C"
    blocks are transparent. Offsets index `clean` (strip_code keeps them)."""
    spans, stack, last = [], [], 0
    for i, ch in enumerate(clean):
        if ch == "{":
            seg = clean[last:i]
            m = re.search(r"\bnamespace\s*([\w:\s]*?)\s*$", seg)
            if m:
                names = [n for n in re.split(r"\s*::\s*", m.group(1).strip())] or [""]
                stack.append((i, names))
            elif re.search(r"\bextern\s*(?:\"\s*\")?\s*$", seg):
                stack.append((i, []))
            else:
                stack.append((i, None))
            last = i + 1
        elif ch == "}":
            if stack:
                o, names = stack.pop()
                if names is not None:
                    spans.append((o, i, names))
            last = i + 1
        elif ch == ";":
            last = i + 1
    return spans


def _chain_at(spans, pos):
    return [n for o, c, names in sorted(spans) if o < pos <= c for n in names]


GATE_RANK = {"OWNGATE_ON": 0, "OWNGATE": 1, "MASTER": 2, "DEVTOOL": 2}      # weakest first


def _brace_close(clean, ob):
    """Index of the '}' matching the '{' at ob in comment/string-blanked text."""
    depth = 0
    for i in range(ob, len(clean)):
        if clean[i] == "{":
            depth += 1
        elif clean[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return None


class GateIndex:
    """B1: gate predicates keyed by DEFINITION (file + namespace), resolved per
    call the way C++ lookup would: a member call is never a free predicate, a
    qualified call needs its namespace, an unqualified call binds the same file's
    definition first, then one in an enclosing (or `using`) namespace. A static /
    anonymous-namespace definition is invisible outside its file. Ambiguity left
    after that resolves to the WEAKEST candidate (safe direction)."""

    CALL_RE = re.compile(r"(?<![\w.:>])((?:\w+\s*::\s*)*)(\w+)\s*\(")

    def __init__(self, parsed, root):
        self.defs = {}               # name -> [dict]
        self.order = {"forcingAllowed": -1}
        self.file_chain = {}         # rel -> (spans, using-namespace names, line starts)
        self.aliases = {}            # rel -> {alias: last component of its target}
        self.using_decl = {}         # rel -> {name: [namespace (last component)]}
        self._masters = {}
        self.legacy = build_gate_preds([(l, f) for _, l, f in parsed])
        for k, name in enumerate(self.legacy):
            self.order.setdefault(name, k)
        for path, lines, _f in parsed:
            rel = os.path.relpath(path, root).replace("\\", "/")
            text = "\n".join(lines)
            clean = _aht.strip_code(text)
            spans = _ns_spans(clean)
            using = set(re.findall(r"\busing\s+namespace\s+(?:[\w]+::)*(\w+)\s*;", clean))
            self.file_chain[rel] = (spans, using, [0] + [m.end() for m in re.finditer("\n", text)])
            # namespace aliases (`namespace K = sr::kernel;`): K:: means kernel::
            self.aliases[rel] = {a: t for a, t in re.findall(
                r"\bnamespace\s+(\w+)\s*=\s*(?:::)?(?:\w+\s*::\s*)*(\w+)\s*;", clean)}
            # D4: using-declarations (`using sr::diag::hackOn;`) bring ONE name
            # into this file; `using namespace` is handled above.
            self.using_decl[rel] = {}
            for um in re.finditer(r"\busing\s+(?!namespace\b)(?:::)?((?:\w+\s*::\s*)+)(\w+)\s*;", clean):
                comps = [q for q in re.split(r"\s*::\s*", um.group(1)) if q]
                self.using_decl[rel].setdefault(um.group(2), []).append(comps[-1])
            for m in re.finditer(r"\bbool\s+(\w+)\s*\([^;{)]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{", text):
                if clean[m.start():m.start() + 4] != "bool":
                    continue                          # inside a comment / string
                ob = m.end() - 1
                if FIX["D6"]:
                    # the predicate's OWN body, never a fixed window that spills into
                    # the next function (9 helpers were misread that way)
                    cb = _brace_close(clean, ob)
                    w = text[m.end():cb] if cb else text[m.end():m.end() + 500]
                    w600 = w
                else:
                    w, w600 = text[m.end():m.end() + 500], text[m.end():m.end() + 600]
                name = m.group(1)
                cls = env = None
                g = re.search(r'getenv\(\s*"(SR_[A-Z0-9_]+)"', w)
                if g:
                    env = g.group(1)
                    default_on = bool(re.search(r"!\s*e\s*\|\|", w)) and not re.search(
                        r"e\s*&&\s*e\[0\]\s*==\s*'1'", w)
                    cls = ("MASTER" if env == "SR_ALLOW_FORCING"
                           else "OWNGATE_ON" if default_on else "OWNGATE")
                if _pred_requires_master(w600):
                    cls, env = "MASTER", "SR_ALLOW_FORCING"
                if cls is None and not FIX["D6"]:
                    continue
                chain = _chain_at(spans, m.start())
                local = "" in chain or bool(re.search(
                    r"\bstatic\s+(?:inline\s+|constexpr\s+)*$", clean[max(0, m.start() - 40):m.start()]))
                # D6: EVERY bool function is a candidate; SiteGates evaluates its
                # body (path + ob) for the real default. cls/env are the legacy
                # heuristic, used only with D6 reverted.
                self.defs.setdefault(name, []).append(
                    {"rel": rel, "path": path, "ob": ob, "chain": [c for c in chain if c],
                     "local": local, "cls": cls, "env": env})

    def chain_of(self, rel, line_idx):
        spans, _u, starts = self.file_chain.get(rel, ([], set(), [0]))
        pos = starts[min(line_idx, len(starts) - 1)]
        return [c for c in _chain_at(spans, pos) if c]

    @staticmethod
    def _weakest(cands):
        cands = [d for d in cands if d["cls"]]
        if not cands:
            return None
        c = min(cands, key=lambda d: GATE_RANK[d["cls"]])
        return c["cls"], c["env"]

    def resolve_defs(self, qual, name, rel, site_chain):
        """B1: the candidate DEFINITIONS a call `qual::name(` at rel binds to (C++
        lookup approximated; ambiguity keeps every candidate so the caller can
        take the weakest). D4: a using-declaration in rel names the namespace."""
        cands = self.defs.get(name)
        if not cands:
            return []
        quals = [q for q in re.split(r"\s*::\s*", qual.strip()) if q]
        same = [d for d in cands if d["rel"] == rel]
        vis = [d for d in cands if d["rel"] != rel and not d["local"]]
        if quals:
            q = self.aliases.get(rel, {}).get(quals[-1], quals[-1])
            return [d for d in same if q in d["chain"]] or [d for d in vis if q in d["chain"]]
        if same:
            return same
        using = self.file_chain.get(rel, ([], set(), []))[1]
        enclosing = [d for d in vis if d["chain"] == site_chain[:len(d["chain"])]]
        declared = []
        if FIX["D4"]:
            for q in self.using_decl.get(rel, {}).get(name, []):
                declared += [d for d in vis if q in d["chain"] and d not in declared]
        if enclosing:
            best = max(len(d["chain"]) for d in enclosing)
            return [d for d in enclosing if len(d["chain"]) == best] + [
                d for d in declared if d not in enclosing]
        if declared:
            return declared
        return [d for d in vis if d["chain"] and d["chain"][-1] in using]

    def resolve(self, qual, name, rel, site_chain):
        if name == "forcingAllowed":
            return ("MASTER", "SR_ALLOW_FORCING")
        if FIX["D40"] and is_devtool_pred(name):
            return ("DEVTOOL", "SR_DEVTOOLS")
        if not FIX["B1"]:
            return self.legacy.get(name)
        hit = self.resolve_defs(qual, name, rel, site_chain)
        return self._weakest(hit) if hit else None

    def body_gate(self, body, rel, site_chain):
        """(cls, env) of the gate predicate(s) the body calls, or None."""
        hits = []
        if FIX["B1"]:
            for m in self.CALL_RE.finditer(body):
                if m.start() >= 2 and body[m.start() - 2:m.start()] == "->":
                    continue
                r = self.resolve(m.group(1), m.group(2), rel, site_chain)
                if r:
                    hits.append((m.group(2), r))
        else:
            for name, r in self.legacy.items():
                if re.search(r"\b" + re.escape(name) + r"\s*\(", body):
                    hits.append((name, r))
        if not hits:
            return None
        if FIX["B2"]:
            # B2: the master gate anywhere in the body wins over an own-gate helper
            # (apt_as_watch: watchEnabled() logs, autoFrameEnabled() gates the write)
            for _n, r in hits:
                if r[0] == "MASTER":
                    return r
            if FIX["D5"]:
                # D5: several own gates -> the WEAKEST (default-ON) one, not the
                # first in the text (legacy whole-body path only; D2 replaces it)
                return min((r for _n, r in hits), key=lambda r: GATE_RANK[r[0]])
            return hits[0][1]
        return min(hits, key=lambda h: self.order.get(h[0], 1 << 30))[1]

    def masters_for(self, rel, site_chain):
        key = (rel, tuple(site_chain))
        if key not in self._masters:
            self._masters[key] = {"forcingAllowed"} | {
                n for n in self.defs if (self.resolve("", n, rel, site_chain) or ("",))[0] == "MASTER"}
        return self._masters[key]


def func_name(sig):
    if FIX["D22"]:
        sig = _ATTR_SPEC_RE.sub(" ", sig)
    if FIX["D9"]:
        # PPC_FUNC(sub_X) / PPC_FUNC_IMPL(__imp__sub_X) define sub_X, not "PPC_FUNC"
        # (the macro token is referenced everywhere, so caller propagation
        # through "PPC_FUNC" reached arbitrary unrelated code)
        m = re.search(r"\bPPC_FUNC(?:_IMPL)?\s*\(\s*(\w+)\s*\)\s*\{?\s*$", sig)
        if m:
            return m.group(1)
    m = re.search(r"(\w+)\s*\([^)]*\)\s*(?:const\s*)?(?:noexcept\s*)?$", sig)
    if not m:
        m = re.search(r"(\w+)\s*\(", sig)
    return (m.group(1) if m else sig)[:60]


def enclosing_func(funcs, line_idx):
    """(start, end, name) of the top-level function containing line_idx, or None."""
    for s, e, sig in funcs:
        if s <= line_idx <= e:
            return s, e, func_name(sig)
    return None


_EXPORT_LAMBDA_RE = re.compile(r'\bregisterExport\s*\(\s*(?:"(\w*)"|(\w+))\s*,\s*\+?\s*\[')


def export_lambdas(lines):
    """B6: [(start_line, end_line, export name)] of lambdas passed straight to
    registerExport("X", [..](..) { .. }). A variable name stands in for X when the
    export is registered through a macro (`registerExport(nm, +[](..){..})`)."""
    text = "\n".join(lines)
    clean = _aht.strip_code(text)
    starts = [0] + [m.end() for m in re.finditer("\n", text)]

    def line_of(pos):
        import bisect
        return bisect.bisect_right(starts, pos) - 1

    out = []
    for m in _EXPORT_LAMBDA_RE.finditer(text):
        if clean[m.start():m.start() + 14] != "registerExport":
            continue                                   # in a comment / string
        depth, i = 0, m.end() - 1
        while i < len(clean):                          # the capture list [...]
            if clean[i] == "[":
                depth += 1
            elif clean[i] == "]":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        ob = clean.find("{", i)
        if ob < 0 or ";" in clean[i:ob]:
            continue
        depth = 0
        for j in range(ob, len(clean)):
            if clean[j] == "{":
                depth += 1
            elif clean[j] == "}":
                depth -= 1
                if depth == 0:
                    out.append((line_of(ob), line_of(j), m.group(1) or m.group(2)))
                    break
    return out


class AuditCtx:
    """Everything classify() needs about the scanned tree."""

    def __init__(self, parsed, root):
        self.root = root
        self.parsed = parsed
        self.gates = GateIndex(parsed, root)
        self.exports = build_export_impls([(l, f) for _, l, f in parsed])
        self.lambdas = {path: export_lambdas(lines) for path, lines, _f in parsed}
        self.by_path = {path: (lines, funcs) for path, lines, funcs in parsed}
        self._refs = None
        self._src = {}
        self._helper_memo = {}
        self._reach_memo = {}
        self._launches = {}
        self._ext = None
        self._ext_text = {}
        self._ext_src = {}
        self.gates_site = SiteGates(self)
        # D9: functions the GUEST calls (a PPCContext& signature, PPC_FUNC):
        # generated code / dispatch tables reach them, the reference index
        # does not (PPC_allocator_slot0_stub is dispatched through a fake VA)
        self.guest_callable = {func_name(sig) for _p, _l, fs in parsed for _s, _e, sig in fs
                               if re.search(r"\bPPCContext\s*&|\bPPC_FUNC(?:_IMPL)?\s*\(", sig)}
        self.prim_rx = {k: rx for k, rx in all_prims() + [HOST_SYNC_PRIM]}

    def launch_lambda_at(self, path, pos):
        """(open, close) of the innermost LAMBDA handed to a host launch
        (std::thread, CreateThread, timers, ...) that contains pos, or None."""
        if path not in self._launches:
            spans = []
            for l in _aht.find_launches(self.src(path)):
                if l.entry == "lambda":
                    spans += l.body_ranges
            self._launches[path] = spans
        best = None
        for a, b in self._launches[path]:
            if a <= pos <= b and (best is None or b - a < best[1] - best[0]):
                best = (a, b)
        return best

    def rel(self, path):
        return os.path.relpath(path, self.root).replace("\\", "/")

    # ---- reference index: identifier -> [(path, line_idx, clean line)]
    def refs(self, name):
        if self._refs is None:
            self._refs = {}
            for path, lines, _f in self.parsed:
                in_block = False
                for idx, line in enumerate(lines):
                    clean, in_block = strip_for_braces(line, in_block)
                    for tok in set(re.findall(r"[A-Za-z_]\w*", clean)):
                        self._refs.setdefault(tok, []).append((path, idx, clean))
        return self._refs.get(name, [])

    def sigs_named(self, name):
        if not hasattr(self, "_sigs"):
            self._sigs = {}
            for _p, _l, fs in self.parsed:
                for _s, _e, sig in fs:
                    self._sigs.setdefault(func_name(sig), []).append(sig)
        return self._sigs.get(name, [])

    # ---- D17: references the classifier's own files cannot show
    def external_refs(self, name):
        """[(label, src, pos)] of references to `name` from GENERATED guest code
        (the committed splice bodies tools/regen/splice_bodies.json, the local
        guest/ppc sr::kernel:: calls), the hand-written guest dir and headers.
        A host function called only from a generated-code hand edit (verifier
        p6, walkStaticCtors_impl) is reached every time the guest reaches the
        edit; round 3 called it "dead code"."""
        if self._ext is None:
            self._ext = {}
            self._ext_text = {}
            root = self.root

            def add(label, text, rx, keep=None):
                # the RAW text is kept (SiteGates reads env names from string
                # literals); matching runs on the comment/string-blanked copy
                self._ext_text[label] = text
                for m in rx.finditer(_aht.strip_code(text)):
                    if keep is None or m.group(1) in keep:
                        self._ext.setdefault(m.group(1), []).append((label, m.start(1)))
            ident = re.compile(r"(?<![\w.>])(?:sr\s*::\s*kernel\s*::\s*)?([A-Za-z_]\w*)\b")
            try:
                bodies = json.load(open(os.path.join(root, SPLICE_BODIES), encoding="utf-8"))
            except (OSError, ValueError):
                bodies = {}
            # generated code names guest functions sub_X / __imp__sub_X itself;
            # any OTHER host function named in a splice body is a hand edit
            host = {func_name(sig) for _p, _l, fs in self.parsed for _s, _e, sig in fs}
            host = {h for h in host if re.fullmatch(r"[A-Za-z_]\w{2,}", h)
                    and not re.match(r"sub_[0-9A-Fa-f]{8}$|__imp__", h)}
            for nm in sorted(bodies):
                if isinstance(bodies[nm], str) and host:
                    add(os.path.join(root, SPLICE_BODIES) + "#" + nm, bodies[nm], ident, host)
            scanned = {os.path.normcase(os.path.abspath(p)) for p, _l, _f in self.parsed}
            extra = []
            hand = os.path.join(root, "tools", "regen", "hand")
            if os.path.isdir(hand):
                extra += [os.path.join(hand, f) for f in sorted(os.listdir(hand)) if f.endswith((".cpp", ".h", ".hpp"))]
            for d in SCAN_DIRS + [GUEST_HAND_DIR]:
                base = os.path.join(root, d)
                if not os.path.isdir(base):
                    continue
                walk = os.walk(base) if d != GUEST_HAND_DIR else [(base, [], os.listdir(base))]
                for dp, _dn, fs in walk:
                    extra += [os.path.join(dp, f) for f in sorted(fs) if f.endswith((".h", ".hpp", ".inl"))]
            for p in extra:
                if os.path.normcase(os.path.abspath(p)) in scanned:
                    continue
                try:
                    add(p, open(p, encoding="utf-8", errors="replace").read(), ident)
                except OSError:
                    pass
            gen = os.path.join(root, GENERATED_DIR)
            if os.path.isdir(gen):
                q = re.compile(r"\bsr\s*::\s*kernel\s*::\s*([A-Za-z_]\w*)")
                for f in sorted(os.listdir(gen)):
                    if f.endswith((".cpp", ".h")):
                        p = os.path.join(gen, f)
                        try:
                            t = open(p, encoding="utf-8", errors="replace").read()
                        except OSError:
                            continue
                        if "sr::kernel::" in t:
                            add(p, t, q)
        return [(label, pos) for label, pos in self._ext.get(name, [])]

    def external_ref_class(self, name, label, pos):
        """(cls, detail) of one external reference: generated / splice code is
        judged by SiteGates over its enclosing function body; a header or other
        hand-written reference runs unconditionally for all we know."""
        text = self._ext_text[label]
        ls = text.rfind("\n", 0, pos) + 1
        le = text.find("\n", pos)
        line = text[ls:le if le >= 0 else len(text)]
        if _proto_re(name).match(line):
            return None                          # a declaration
        if label.endswith((".h", ".hpp", ".inl")) and "#" not in label:
            if re.search(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*(?:const\s*)?\{", text[ls:ls + 400]):
                return None                      # its own inline definition head
        gen = "#" in label or os.sep + "ppc" + os.sep in label or "/ppc/" in label.replace("\\", "/")
        where = os.path.relpath(label.split("#")[0], self.root).replace("\\", "/") + (
            "#" + label.split("#", 1)[1] if "#" in label else "") + f":{text.count(chr(10), 0, pos) + 1}"
        if not gen:
            return "UNGATED", f"referenced from hand-written header/guest code {where}"
        # the enclosing function of the generated reference
        fo = max(text.rfind("\nPPC_FUNC_IMPL(", 0, pos), text.rfind("\nPPC_FUNC(", 0, pos))
        lo = fo + 1 if fo >= 0 else 0
        clean = _aht.strip_code(text)
        ob = clean.find("{", lo)
        cb = _brace_close(clean, ob) if ob >= 0 else None
        body = text[lo:cb + 1] if cb is not None and cb >= pos else text
        off = pos - lo if body is not text else pos
        key = (label, lo)
        src = self._ext_src.get(key)
        if src is None:
            src = self._ext_src[key] = TextSrc(os.path.join(self.root, "_ext_", str(len(self._ext_src))), body, self.root)
            self._src[src.path] = src
        lvl, env, _c = self.gates_site.level_at(src, off, inherited=True)
        tag = f"reached from generated guest code {where}"
        if lvl == DEAD:
            return "OWNGATE", "observe-only(dead) " + tag
        if lvl == MASTER:
            return "MASTER", tag
        if lvl == DEVTOOL:
            return "DEVTOOL", f"{env} " + tag
        if lvl == OFF:
            return "OWNGATE", f"{env} " + tag
        if lvl == ON:
            return "OWNGATE_ON", f"{env} " + tag
        return "UNGATED", tag

    def export_lambda_at(self, path, line_idx):
        best = None
        for s, e, name in self.lambdas.get(path, []):
            if s <= line_idx <= e and (best is None or e - s < best[1] - best[0]):
                best = (s, e, name)
        return best

    # ---- B7: a helper whose every reference is a CALL from export code
    def export_of(self, fname, depth=0):
        if fname in self.exports:
            return self.exports[fname]
        if not FIX["B7"] or depth > 4 or not re.match(r"^\w+$", fname or ""):
            return None
        if fname in self._helper_memo:
            return self._helper_memo[fname]
        self._helper_memo[fname] = None                        # cycle guard
        call_re = re.compile(r"\b" + re.escape(fname) + r"\s*\(")
        found = None
        for path, idx, clean in self.refs(fname):
            if _proto_re(fname).match(clean):
                continue
            lines, funcs = self.by_path[path]
            enc = enclosing_func(funcs, idx)
            if enc is None and _def_head(funcs, idx, fname):
                continue                    # the definition's signature line
            if enc is not None and enc[2] == fname:
                continue                    # its own body (recursion)
            if enc is None or not call_re.search(clean):
                found = None                # thread entry / callback: its own context
                break
            if FIX["D3"]:
                s = self.src(path)
                seg = s.clean[s._line_starts[idx]:s._line_starts[idx + 1]
                              if idx + 1 < len(s._line_starts) else len(s.clean)]
                ms = list(re.finditer(r"\b" + re.escape(fname) + r"\s*\(", seg))
                if not FIX["D12"]:
                    ms = ms[:1]
                if any(self.launch_lambda_at(path, s._line_starts[idx] + m.start()) for m in ms):
                    found = None            # called from a thread the export starts
                    break
                if FIX["D14"] and any(self.gates_site.level_at(s, s._line_starts[idx] + m.start(),
                                                               inherited=True)[0] >= OFF for m in ms):
                    # D14: an export calls it only under a gate (the VdSwap
                    # fake-completion shape one level down): not the export's
                    # out-param contract - caller propagation judges it
                    found = None
                    break
            lam = self.export_lambda_at(path, idx) if FIX["B6"] else None
            exp = lam[2] if lam else self.export_of(enc[2], depth + 1)
            if exp is None:
                found = None
                break
            found = found or exp
        self._helper_memo[fname] = found
        return found

    # ---- B4: every reference to fname only runs with the master gate on
    def src(self, path):
        if path not in self._src:
            self._src[path] = _aht.Src(path, self.root)
        return self._src[path]

    def master_reached(self, fname, depth=0):
        """(True, via) when every reference to fname (call or thread/callback
        entry) sits inside a master-gated branch / after a master early-return,
        or in a function that is itself only master-reached."""
        if depth > 5 or not re.match(r"^\w+$", fname or ""):
            return False, ""
        if fname in self._reach_memo:
            return self._reach_memo[fname]
        self._reach_memo[fname] = (False, "")                   # cycle guard
        n, via = 0, ""
        for path, idx, clean in self.refs(fname):
            if _proto_re(fname).match(clean):
                continue
            lines, funcs = self.by_path[path]
            enc = enclosing_func(funcs, idx)
            if enc is None and _def_head(funcs, idx, fname):
                continue                    # the definition's signature line
            if enc is not None and enc[2] == fname:
                continue                    # its own body (recursion)
            if enc is None:                 # file-scope use (a global initializer): ungated
                self._reach_memo[fname] = (False, "")
                return False, ""
            n += 1
            s = self.src(path)
            ls = s._line_starts[idx]
            le = s._line_starts[idx + 1] if idx + 1 < len(s._line_starts) else len(s.clean)
            m = re.search(r"\b" + re.escape(fname) + r"\b", s.clean[ls:le])
            rel = self.rel(path)
            if m and FIX["D2"] and FIX["D12"]:
                # D12: EVERY reference on the line must be master-gated
                gated = all(self.gates_site.level_at(s, ls + q.start())[0] >= MASTER
                            for q in re.finditer(r"\b" + re.escape(fname) + r"\b", s.clean[ls:le]))
            elif m and FIX["D2"]:
                gated = self.gates_site.level_at(s, ls + m.start())[0] >= MASTER
            else:
                gated = m and _aht.gate_of(s, ls + m.start(), self.gates.masters_for(
                    rel, self.gates.chain_of(rel, idx)))
            if gated:
                via = via or f"{rel}:{idx + 1} in {enc[2]}()"
                continue
            ok, v = self.master_reached(enc[2], depth + 1)
            if not ok:
                self._reach_memo[fname] = (False, "")
                return False, ""
            via = via or v
        res = (n > 0, via if n else "")
        self._reach_memo[fname] = res
        return res


def _def_head(funcs, idx, fname):
    """A file-scope line that is the signature head of fname's own definition."""
    return any(0 <= s - idx <= 6 and func_name(sig) == fname for s, _e, sig in funcs)


_PROTO_CACHE = {}


def _proto_re(fname):
    # A prototype needs a TYPE token before the name (`static void F(int);`);
    # a call statement `  F(h);` / `return F(x);` is a reference.
    r = _PROTO_CACHE.get(fname)
    if r is None:
        r = _PROTO_CACHE[fname] = re.compile(
            r"^\s*(?!return\b|else\b|case\b|throw\b)(?:(?:static|extern|inline)\s+)*"
            r"[\w:<>]+[\s\*&]+(?:[\w:<>]+[\s\*&]+)*" + re.escape(fname) + r"\s*\([^)]*\)\s*;")
    return r


# ---------------------------------------------------------------------------
# iter-463 round 2 (D1/D2/D5/D6): SITE-LEVEL gate analysis.
#
# The question is "can this write run in the default exe, and which switch
# decides?", asked AT THE WRITE: every condition the site needs -- enclosing
# if / else / while heads, brace-less `if (C) stmt`, and early-exit guards
# (`if (C) <branch that always exits>` before the site in an enclosing block) --
# is evaluated to a LEVEL:
#   NONE   runs by default, no switch     -> UNGATED (then caller propagation)
#   ON     a private SR_* switch, runs by default          -> OWNGATE_ON
#   OFF    a private SR_* switch, off by default (opt-in)  -> OWNGATE
#   MASTER runs only under forcingAllowed()                -> MASTER
#   DEAD   never runs (constant false, kObserveOnly)       -> OWNGATE (dead)
# All conditions must hold, so conditions combine by the STRONGEST (max); an
# `a || b` holds when either does, so it takes the WEAKEST (min). Negation maps
# ON<->OFF and MASTER->NONE (!forcingAllowed() gates nothing). A gate predicate
# (bool helper) is evaluated from its own body: the weakest of its `return`s,
# each combined with the guards in front of it. Anything not understood is NONE
# (the safe direction: it can only make a site look MORE ungated).
#
# LIMITS (documented, all in the safe direction unless noted): short-circuit
# `gate() && write()` inside one expression, ternaries and switch/case values
# are not gates; a guard in a callee (a predicate the site does not test) is not
# seen; bool variables that are assigned more than once or have their address
# taken are unknown (NONE); `if (A) if (B) x; else site` sees only !B.
# iter-463 phase 2: DEVTOOL sits between MASTER and DEAD - gated (it never runs by default)
# and reported in its own bucket.
NONE, ON, OFF, MASTER, DEVTOOL, DEAD = 0, 1, 2, 3, 4, 5
CLS_LVL = {"OWNGATE_ON": ON, "OWNGATE": OFF, "MASTER": MASTER, "DEVTOOL": DEVTOOL}
V_NONE = (NONE, "", False)        # (level, env, kill-switch provenance)
_SWAP = {ON: OFF, OFF: ON}


def _v_and(vals):
    """Conjunction: every operand must hold -> the strongest requirement."""
    vals = list(vals)
    if not vals:
        return V_NONE
    if not FIX["B2"]:
        return vals[0]                      # B2 reverted: the first operand won
    return max(vals, key=_rank)


def _v_or(vals):
    """Disjunction: either operand is enough -> the weakest requirement."""
    vals = list(vals)
    if not vals:
        return V_NONE
    if not FIX["D5"]:
        return vals[0]                      # D5 reverted: the first operand won
    return min(vals, key=_rank)


def _v_max(vals):
    vals = list(vals)
    return max(vals, key=_rank) if vals else V_NONE


def _rank(v):
    # a kill-switch ON is weaker than a real default-ON switch
    return (v[0], not v[2])


def _neg_of(v):
    """(when true) -> (when false) for an opaque predicate / env term. `!off`
    is ON, but only as a KILL SWITCH: code that runs unless an opt-in (test /
    diagnostic) mode is set. At the site that is its own A/B switch; inherited
    from a caller (`if (testMode) return; runHarness();`) it gates nothing."""
    if v[0] == OFF:
        return (ON, v[1], True)
    if v[0] == ON:
        return (OFF, v[1], False)
    return V_NONE


def _ws(c, i):
    n = len(c)
    while i < n and c[i].isspace():
        i += 1
    return i


def _trim(c, a, b):
    while a < b and c[a].isspace():
        a += 1
    while b > a and c[b - 1].isspace():
        b -= 1
    return a, b


def _split_top(c, a, b, op):
    parts, depth, last, i = [], 0, a, a
    while i < b:
        ch = c[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif depth == 0 and c.startswith(op, i):
            parts.append((last, i))
            i += len(op)
            last = i
            continue
        i += 1
    parts.append((last, b))
    return parts


def _has_top(c, a, b, ch_set):
    depth = 0
    for i in range(a, b):
        ch = c[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif depth == 0 and ch in ch_set:
            return i
    return -1


def _ident(c, i):
    return 0 <= i < len(c) and (c[i].isalnum() or c[i] == "_")


_EXIT_RE = re.compile(r"(?:return|continue|break|goto|throw)\b|"
                      r"(?:std::)?(?:abort|exit|_exit|quick_exit|terminate)\s*\(|"
                      r"__fastfail\s*\(|ExitProcess\s*\(|TerminateProcess\s*\(")
_LABEL_RE = re.compile(r"(?:case\b[^;:]*|default\s*|(?!else\b|return\b)[A-Za-z_]\w*\s*):(?!:)")
_IF_RE = re.compile(r"if\s*(?:constexpr\s*)?\(")
_ENV_DECL_RE = re.compile(r"\b(\w+)\s*=\s*(?:::)?(?:std\s*::\s*)?getenv\s*\(\s*\"")
_GETENV_RE = re.compile(r"(?:::)?(?:std\s*::\s*)?\bgetenv\s*\(\s*\"")
# `bool envOn(const char* name) { const char* e = getenv(name); ... }`: the env
# is the CALLER's string literal (srGateDefaultOn("SR_X"), envOn("SR_KPCR")).
PARAM_ENV = "<arg>"
_ENV_PARAM_DECL_RE = re.compile(r"\b(\w+)\s*=\s*(?:::)?(?:std\s*::\s*)?getenv\s*\(\s*[A-Za-z_]\w*\s*\)")
_GETENV_PARAM_RE = re.compile(r"(?:::)?(?:std\s*::\s*)?\bgetenv\s*\(\s*[A-Za-z_]\w*\s*\)")
_DECL_ATOM_RE = re.compile(r"(?:(?:const|static|constexpr|volatile)\s+)*[A-Za-z_][\w:]*(?:\s*<[^=]*>)?"
                           r"(?:\s*[\*&]+\s*|\s+)(?:const\s+)?[A-Za-z_]\w*\s*=(?!=)")


class SiteGates:
    """Site-level gate evaluator over audit_host_threads.Src files."""

    def __init__(self, ctx):
        self.ctx = ctx
        self._stmts, self._pred, self._var, self._env, self._fb = {}, {}, {}, {}, {}
        self._decl_index = {}
        self._c2o = {}

    # ---- structure helpers
    def _close_to_open(self, src):
        m = self._c2o.get(src.path)
        if m is None:
            m = self._c2o[src.path] = {v: k for k, v in src.brace_match.items()}
        return m

    def _stmt_start(self, src, p, floor):
        """Start of the statement containing p inside block `floor` (paren aware).
        Second value: p sits inside an enclosing ( or [ (expression context)."""
        c, depth, k = src.clean, 0, p - 1
        while k > floor:
            ch = c[k]
            if ch in ")]":
                depth += 1
            elif ch in "([":
                # p is an ARGUMENT (std::thread(fn), registerExport("X", [..]{..}),
                # if (a && write())): keep walking to the statement start; the
                # prefix parser drops any condition that still contains p
                if depth > 0:
                    depth -= 1
            elif depth == 0 and ch in ";{}":
                return k + 1, False
            k -= 1
        return floor + 1, False

    def _stmt_end(self, src, i, limit=None):
        """Index of the ';' (or closing '}') that ends the simple statement at i."""
        c = src.clean
        limit = len(c) if limit is None else limit
        depth = 0
        while i < limit:
            ch = c[i]
            if ch in "([":
                depth += 1
            elif ch in ")]":
                depth -= 1
            elif ch == "{":
                cb = src.brace_match.get(i)
                if cb is None:
                    return limit
                if depth == 0:
                    k = _ws(c, cb + 1)
                    if k < limit and (c[k] in ";,)]." or c[k] == "(" or re.match(r"(?:else|catch)\b", c[k:k + 6])):
                        i = cb + 1
                        continue
                    return cb
                i = cb + 1
                continue
            elif ch == ";" and depth == 0:
                return i
            i += 1
        return limit

    def top_stmts(self, src, ob):
        """[(start, end)] of the statements directly inside block ob."""
        key = (src.path, ob)
        if key in self._stmts:
            return self._stmts[key]
        c, end = src.clean, src.brace_match.get(ob, ob)
        out, i = [], ob + 1
        while True:
            i = _ws(c, i)
            if i >= end:
                break
            is_do = c.startswith("do", i) and not _ident(c, i + 2)
            j, depth = i, 0
            while j < end:
                ch = c[j]
                if ch in "([":
                    depth += 1
                elif ch in ")]":
                    depth -= 1
                elif ch == "{":
                    cb = src.brace_match.get(j)
                    if cb is None:
                        j = end
                        break
                    if depth > 0:
                        j = cb + 1
                        continue
                    k = _ws(c, cb + 1)
                    if k < end and (c[k] in ";,)].(" or re.match(r"(?:else|catch)\b", c[k:k + 6])
                                    or (is_do and c.startswith("while", k))):
                        j = cb + 1
                        continue
                    j = cb                          # the compound statement ends here
                    break
                elif ch == ";" and depth == 0:
                    break
                j += 1
            out.append((i, min(j, end)))
            i = j + 1
        self._stmts[key] = out
        return out

    def _skip_labels(self, c, i, end):
        """Skip case/default/goto labels (saw=True) and preprocessor lines
        (`#ifdef _WIN32` in front of `if (forcingAllowed()) {`)."""
        saw = False
        while True:
            i = _ws(c, i)
            if i < end and c[i] == "#":
                while True:                         # to end of line, honouring `\`
                    nl = c.find("\n", i)
                    if nl < 0 or nl >= end:
                        return end, saw
                    if c[nl - 1] != "\\":
                        break
                    i = nl + 1
                i = nl + 1
                continue
            m = _LABEL_RE.match(c, i, end)
            if not m:
                return i, saw
            i, saw = m.end(), True

    def _is_lambda_block(self, src, ob):
        c = src.clean
        k = ob - 1
        while k >= 0 and c[k].isspace():
            k -= 1
        head = c[max(0, k - 200):k + 1]
        head = re.sub(r"(?:->\s*[\w:<>\*&\s]+|\bmutable|\bnoexcept|\bconstexpr|\s)+$", "", head)
        if head.endswith("]"):
            return True
        if head.endswith(")"):
            o = _open_paren(c, max(0, k - 200) + len(head) - 1)
            if o is not None:
                j = o - 1
                while j >= 0 and c[j].isspace():
                    j -= 1
                return j >= 0 and c[j] == "]"
        return False

    def func_block(self, src, pos):
        """Outermost enclosing block that is not a namespace / extern / class."""
        chain = src.enclosing(pos)
        key = (src.path, chain[0] if chain else -1)
        if key in self._fb:
            return self._fb[key]
        res = -1
        for ob in reversed(chain):
            st, _ = self._stmt_start(src, ob, src.parent.get(ob, -1))
            head = src.clean[st:ob]
            if re.search(r"\bnamespace\b|\bextern\b|\b(?:struct|class|union|enum)\b[^;(){}]*$", head):
                continue
            res = ob
            break
        self._fb[key] = res
        return res

    # ---- the requirements in front of a position
    def _cond_of_if(self, src, i):
        """i at `if`: (cond_a, cond_b, close) or None."""
        m = _IF_RE.match(src.clean, i)
        if not m:
            return None
        o = i + m.end() - m.start() - 1
        close = src._match_paren(o)
        return None if close is None else (o + 1, close, close)

    def _prefix_reqs(self, src, p, floor):
        """Conditions of the statement prefix in front of p inside block floor:
        `if (C) ...p`, `while (C) ...p`, `else ...p`."""
        c = src.clean
        st, inexpr = self._stmt_start(src, p, floor)
        if inexpr:
            return []
        reqs = []
        i, _ = self._skip_labels(c, st, p)
        if c.startswith("else", i) and not _ident(c, i + 4):
            reqs += self._else_reqs(src, st - 1, floor)
            i = _ws(c, i + 4)
        while i < p:
            i, _ = self._skip_labels(c, i, p)
            m = re.compile(r"(if|while|for|switch)\s*(?:constexpr\s*)?\(|do\b|try\b|else\b").match(c, i)
            if not m:
                break
            if m.group(1):
                o = m.end() - 1
                close = src._match_paren(o)
                if close is None or close >= p:
                    break
                if m.group(1) in ("if", "while"):
                    reqs.append((o + 1, close, True))
                i = _ws(c, close + 1)
            else:
                i = _ws(c, m.end())
        return reqs

    def _else_reqs(self, src, b, floor, depth=0):
        """`else` after boundary b: the preceding if's condition is false."""
        c = src.clean
        if b < 0 or depth > 20:
            return []
        if c[b] == "}":
            o = self._close_to_open(src).get(b)
            if o is None:
                return []
            st, inexpr = self._stmt_start(src, o, floor)
            end = o
        elif c[b] == ";":
            st, inexpr = self._stmt_start(src, b, floor)
            end = b
        else:
            return []
        if inexpr:
            return []
        i, _ = self._skip_labels(c, st, end)
        reqs = []
        if c.startswith("else", i) and not _ident(c, i + 4):
            reqs += self._else_reqs(src, st - 1, floor, depth + 1)   # else-if chain
            i = _ws(c, i + 4)
        last = None
        while i < end:
            m = re.compile(r"(if|while|for|switch)\s*(?:constexpr\s*)?\(").match(c, i)
            if not m:
                break
            o = m.end() - 1
            close = src._match_paren(o)
            if close is None or close >= end:
                break
            if m.group(1) == "if":
                last = (o + 1, close)
            i = _ws(c, close + 1)
        if last:
            reqs.append((last[0], last[1], False))
        return reqs

    def _branch(self, src, i, limit):
        """(end, exits) of the if/else branch starting at i."""
        c = src.clean
        i = _ws(c, i)
        if i < len(c) and c[i] == "{":
            cb = src.brace_match.get(i)
            if cb is None:
                return limit, False
            return cb, self.block_exits(src, i)
        end = self._stmt_end(src, i, limit)
        return end, self.stmt_exits(src, i, end)

    def stmt_exits(self, src, s, e):
        c = src.clean
        i, _ = self._skip_labels(c, s, e)
        if _EXIT_RE.match(c, i):
            return True
        if i < len(c) and c[i] == "{":
            return self.block_exits(src, i)
        g = self._cond_of_if(src, i)
        if g:
            t_end, t_exit = self._branch(src, g[2] + 1, e + 1)
            k = _ws(c, t_end + 1)
            if c.startswith("else", k) and not _ident(c, k + 4):
                _e_end, e_exit = self._branch(src, k + 4, len(c))
                return t_exit and e_exit
        return False

    def block_exits(self, src, ob):
        """D1: the block ALWAYS leaves the enclosing code: one of its own
        statements (not a nested if's) is return/continue/break/goto/throw/abort,
        or an if/else whose both branches always exit."""
        if not FIX["D1"]:
            inner = src.clean[ob + 1:src.brace_match.get(ob, ob)]
            return bool(re.search(r"\b(?:return|continue|break|goto)\b[^;{}]*;\s*$", inner))
        return any(self.stmt_exits(src, s, e) for s, e in self.top_stmts(src, ob))

    def _exit_guard_reqs(self, src, ob, p):
        """Early-exit guards before p among block ob's own statements."""
        c = src.clean
        reqs, stmts = [], self.top_stmts(src, ob)
        for s, e in stmts:
            if s >= p:
                break
            i, saw_label = self._skip_labels(c, s, min(e, p))
            if saw_label:
                reqs = []                   # a case/goto label: control can enter here
            if e >= p:
                break                       # the statement holding p (prefix handles it)
            g = self._cond_of_if(src, i)
            if not g:
                continue
            t_end, t_exit = self._branch(src, g[2] + 1, e + 1)
            if t_end >= p:
                continue                    # p is inside the then-branch
            k = _ws(c, t_end + 1)
            has_else = c.startswith("else", k) and not _ident(c, k + 4)
            e_exit = False
            if has_else:
                j = _ws(c, k + 4)
                if self._cond_of_if(src, j):
                    continue                # else-if chain: not a plain guard
                e_end, e_exit = self._branch(src, j, len(c))
                if e_end >= p:
                    continue                # p is inside the else-branch
            if t_exit and not e_exit:
                reqs.append((g[0], g[1], False))
            elif e_exit and not t_exit:
                reqs.append((g[0], g[1], True))
        return reqs

    def requirements(self, src, pos, stop_ob=None):
        """[(a, b, polarity)]: conditions that must hold for pos to execute,
        walking out to the function (or stop_ob)."""
        chain = src.enclosing(pos)
        reqs = self._prefix_reqs(src, pos, chain[0] if chain else -1)
        for k, ob in enumerate(chain):
            reqs += self._exit_guard_reqs(src, ob, pos)
            if ob == stop_ob:
                break
            reqs += self._prefix_reqs(src, ob, chain[k + 1] if k + 1 < len(chain) else -1)
        return reqs

    def value_at(self, src, pos, stop_ob=None):
        """(level, env, kill) that decides whether pos runs."""
        vals = []
        for a, b, pol in self.requirements(src, pos, stop_ob):
            pv, nv = self.ev(src, a, b)
            vals.append(pv if pol else nv)
        return _v_max(vals)

    def level_at(self, src, pos, stop_ob=None, inherited=False):
        """(level, env, [condition texts]) that decides whether pos runs.
        inherited=True: pos is a CALLER's reference - a kill switch there
        (`if (testMode) return;`) does not gate the callee (D10)."""
        reqs = self.requirements(src, pos, stop_ob)
        vals = []
        for a, b, pol in reqs:
            pv, nv = self.ev(src, a, b)
            v = pv if pol else nv
            if inherited and v[0] == ON and v[2] and FIX["D10"]:
                v = V_NONE
            if v[1] == PARAM_ENV:
                v = V_NONE                  # the env NAME is a runtime value here
            vals.append(v)
        v = _v_max(vals)
        return v[0], v[1], [("" if pol else "!") + "(" + src.clean[a:b].strip() + ")" for a, b, pol in reqs]

    # ---- condition evaluation: (when-true value, when-false value)
    def ev(self, src, a, b, depth=0):
        c = src.clean
        a, b = _trim(c, a, b)
        if a >= b or depth > 60:
            return V_NONE, V_NONE
        parts = _split_top(c, a, b, ";")               # if (init; cond)
        if len(parts) > 1:
            a, b = _trim(c, *parts[-1])
            if a >= b:
                return V_NONE, V_NONE
        if _has_top(c, a, b, "?") >= 0:
            return V_NONE, V_NONE
        ors = _split_top(c, a, b, "||")
        if len(ors) > 1:
            vs = [self.ev(src, x, y, depth + 1) for x, y in ors]
            # !(a || b) == !a && !b  (B3: the guard `if (!M || !own) return`
            # leaves M && own; reverted, it read as a bypass)
            neg = _v_max([v[1] for v in vs]) if FIX["B3"] else _v_or([v[1] for v in vs])
            return _v_or([v[0] for v in vs]), neg
        ands = _split_top(c, a, b, "&&")
        if len(ands) > 1:
            vs = [self.ev(src, x, y, depth + 1) for x, y in ands]
            return _v_and([v[0] for v in vs]), _v_or([v[1] for v in vs])
        if c[a] == "!" and c[a + 1:a + 2] != "=":
            pv, nv = self.ev(src, a + 1, b, depth + 1)
            return nv, pv
        if c[a] == "(" and src._match_paren(a) == b - 1:
            return self.ev(src, a + 1, b - 1, depth + 1)
        return self.atom(src, a, b, depth)

    def atom(self, src, a, b, depth):
        c = src.clean
        if c.startswith("::", a):                   # ::sr::kernel::forcingAllowed()
            a += 2
        t = c[a:b]
        if re.fullmatch(r"true|1|TRUE", t):
            return V_NONE, (DEAD, "", False)
        if re.fullmatch(r"false|0|0u|nullptr|NULL|FALSE", t):
            return (DEAD, "", False), V_NONE
        # comparisons against 0 / nullptr / false / true normalise to the operand
        for op in ("==", "!="):
            k = _find_top_op(c, a, b, op)
            if k >= 0:
                la, lb = _trim(c, a, k)
                ra, rb = _trim(c, k + 2, b)
                lt, rt = c[la:lb], c[ra:rb]
                zero = r"0|0u|nullptr|NULL|false"
                for (xa, xb), other in (((la, lb), rt), ((ra, rb), lt)):
                    if re.fullmatch(zero, other):
                        pv, nv = self.ev(src, xa, xb, depth + 1)
                        return (pv, nv) if op == "!=" else (nv, pv)
                    if other == "true":
                        pv, nv = self.ev(src, xa, xb, depth + 1)
                        return (pv, nv) if op == "==" else (nv, pv)
                break
        env = self._env_ref(src, a, b)
        if env:
            name, bare = env
            if not FIX["D6"]:
                return (OFF, name, False), (OFF, name, False)    # legacy: any getenv = opt-in
            return (((OFF, name, False), (ON, name, True)) if bare
                    else ((OFF, name, False), (OFF, name, False)))
        if _has_top(c, a, b, "<>!=") >= 0 and FIX["D6"]:
            r = self._test_hook_cmp(src, a, t)
            if r:
                return r
        if _has_top(c, a, b, "<>") >= 0 and not re.match(r"[\w:]+\s*<", t):
            return V_NONE, V_NONE                        # relational comparison
        m = _DECL_ATOM_RE.match(t)
        if m and not re.match(r"(?:return|else|case)\b", t):
            return self.ev(src, a + m.end(), b, depth + 1)
        m = re.match(r"((?:\w+\s*::\s*)*)(\w+)\s*\(", t)
        if m and src._match_paren(a + m.end() - 1) == b - 1:
            name = m.group(2)
            if name in ("forcingAllowed", "forcingAllowedFor"):
                return (MASTER, "SR_ALLOW_FORCING", False), V_NONE
            if FIX["D40"] and is_devtool_pred(name):
                # iter-463 phase 2: the tool is the literal argument (hook = the aggregate)
                q = c.find('"', a, b)
                lit = src.raw[q + 1:c.find('"', q + 1)] if q >= 0 else ""
                tool = lit if name == "devToolAllowed" else "hook"
                return (DEVTOOL, "SR_DEVTOOLS:" + (tool or "?"), False), V_NONE
            r = self.pred_call(src, a, m.group(1), name, depth)
            if r and (r[0][1] == PARAM_ENV or r[1][1] == PARAM_ENV):
                # bind the helper's env parameter to this call's string literal
                q = c.find('"', a, b)
                lit = src.raw[q + 1:c.find('"', q + 1)] if q >= 0 else ""
                sub = lambda v: (v if v[1] != PARAM_ENV else
                                 (v[0], lit, v[2]) if re.fullmatch(r"SR_\w+", lit) else V_NONE)
                if lit == "SR_ALLOW_FORCING":
                    r = ((MASTER, lit, False), V_NONE) if r[0][0] > NONE else (V_NONE, V_NONE)
                else:
                    r = (sub(r[0]), sub(r[1]))
            return r if r else (V_NONE, V_NONE)
        m = re.fullmatch(r"((?:\w+\s*::\s*)*)([A-Za-z_]\w*)", t)
        if m:
            r = self.var_value(src, a, m.group(2), depth)
            if r:
                return r
        return V_NONE, V_NONE

    # ---- test-override hooks: `std::atomic<int> g_xTestMode{-1};  // -1 = env`
    # read as `const int m = g_xTestMode.load(); if (m >= 0) return m != 0;`
    # in front of the env switch (kpcrEnabled, obKThreadEnabled, xam_content
    # fsEnabled). In the default exe the hook holds its -1 sentinel, PROVIDED
    # every store to it sits in a function whose name says Test (the
    # `*SetEnabledForTest(int)` convention); any other store -> unknown.
    _CMP_RE = re.compile(r"(-?\s*\d+|[A-Za-z_]\w*)\s*(>=|<=|==|!=|>|<)\s*(-?\s*\d+|[A-Za-z_]\w*)")

    def _test_hook_cmp(self, src, a, t):
        m = self._CMP_RE.fullmatch(t.strip())
        if not m:
            return None
        l, op, r = m.group(1), m.group(2), m.group(3)
        if re.fullmatch(r"-?\s*\d+", l):
            l, r = r, l
            op = {">=": "<=", "<=": ">=", ">": "<", "<": ">"}.get(op, op)
        if not re.fullmatch(r"[A-Za-z_]\w*", l) or not re.fullmatch(r"-?\s*\d+", r):
            return None
        g = self._test_hook_global(src, a, l)
        if g is None:
            return None
        k = int(r.replace(" ", ""))
        holds = {">=": -1 >= k, "<=": -1 <= k, ">": -1 > k, "<": -1 < k,
                 "==": -1 == k, "!=": -1 != k}[op]
        hook = (OFF, "test hook " + g, False)
        return (V_NONE, hook) if holds else (hook, V_NONE)

    def _test_hook_global(self, src, a, name):
        c = src.clean
        fb = self.func_block(src, a)
        g = name
        if fb >= 0:
            for m in re.finditer(r"\b(?:const\s+)?(?:int|auto|int32_t)\s+" + re.escape(name) +
                                 r"\s*=\s*([A-Za-z_]\w*)\s*(?:\.\s*load\s*\([^;]*\))?\s*;", c[fb:a]):
                g = m.group(1)
        key = ("hook", g)
        if key in self._var:
            return self._var[key]
        self._var[key] = None
        ok = False
        for path, idx, clean in self.ctx.refs(g):
            lines, funcs = self.ctx.by_path[path]
            enc = enclosing_func(funcs, idx)
            if re.search(r"(?:std::atomic<\s*int\s*>|\bint)\s+" + re.escape(g) +
                         r"\s*(?:\{\s*-\s*1\s*\}|=\s*-\s*1\s*;)", clean) and enc is None:
                ok = True                               # the -1 sentinel declaration
                continue
            if re.search(r"\b" + re.escape(g) + r"\s*(?:\.\s*(?:store|exchange|fetch_\w+|compare_exchange\w*)\s*\(|"
                         r"(?:=(?!=)|\+=|-=|\+\+|--))|(?:\+\+|--)\s*" + re.escape(g) + r"\b|(?<![&\w)\]])&\s*" +
                         re.escape(g) + r"\b", clean):
                if enc is None or not re.search(r"[Tt]est", enc[2]):
                    self._var[key] = None
                    return None                         # a non-test store
                if FIX["D15"] and self._called_in_tree(enc[2]):
                    # D15: the `*ForTest` setter is called from PRODUCTION code
                    # (the scanned tree; tests live in tools/difftest): its name
                    # proves nothing about the default exe
                    self._var[key] = None
                    return None
        self._var[key] = g if ok else None
        return self._var[key]

    def _env_bindings(self, src, fb):
        key = (src.path, fb)
        if key not in self._env:
            c = src.clean
            lo, hi = (fb, src.brace_match.get(fb, len(c))) if fb >= 0 else (0, len(c))
            env = []
            for m in _ENV_DECL_RE.finditer(c, lo, hi):
                q = m.end()
                name = src.raw[q:c.find('"', q)]
                if re.fullmatch(r"SR_\w+", name):
                    env.append((m.group(1), name, m.start()))
            if FIX["D6"]:
                for m in _ENV_PARAM_DECL_RE.finditer(c, lo, hi):
                    env.append((m.group(1), PARAM_ENV, m.start()))
            self._env[key] = env
        return self._env[key]

    def visible(self, src, decl_pos, name, a):
        """The declaration of `name` at decl_pos is in scope at a and not shadowed."""
        if decl_pos >= a:
            return False
        blk = src.innermost(decl_pos)
        if blk != -1 and blk not in src.enclosing(a):
            return False
        c = src.clean
        shadow = re.compile(r"(?<![\w&|])(?!(?:return|else|case|throw|delete|new|sizeof|co_return)\b)"
                            r"(?:[A-Za-z_][\w:]*|>)(?:\s*\*+\s*|\s*&(?!&)\s*|\s+)"
                            + re.escape(name) + r"\s*(?:[:=;,)\[{(])")
        for m in shadow.finditer(c, decl_pos + 1, a):
            ib = src.innermost(m.start())
            if ib != blk and (ib == -1 or ib in src.enclosing(a)):
                return False
        return True

    def _env_ref(self, src, a, b):
        """(env, bare) when the atom reads an SR_* env var: bare = it only tests
        the pointer (`e`, `getenv("SR_X")`), else it dereferences it."""
        c, t = src.clean, src.clean[a:b]
        m = _GETENV_RE.search(c, a, b)
        if m:
            q = m.end()
            name = src.raw[q:c.find('"', q)]
            if re.fullmatch(r"SR_\w+", name):
                close = src._match_paren(c.find("(", m.start()))
                return name, (m.start() == a and close == b - 1)
        m = _GETENV_PARAM_RE.search(c, a, b) if FIX["D6"] else None
        if m:
            return PARAM_ENV, (m.start() == a and m.end() == b)
        best = None
        for v, name, dpos in self._env_bindings(src, self.func_block(src, a)):
            if re.search(r"(?<![\w.>])" + re.escape(v) + r"\b(?!\s*(?:\.|->))", t) and \
                    self.visible(src, dpos, v, a) and (best is None or dpos > best[2]):
                best = (v, name, dpos)
        if best:
            return best[1], t.strip() == best[0]
        return None

    def pred_call(self, src, a, qual, name, depth):
        g = self.ctx.gates
        rel = self.ctx.rel(src.path)
        if not FIX["B1"]:
            r = g.legacy.get(name)
            if not r:
                return None
            v = (CLS_LVL[r[0]], r[1], False)
            return v, _neg_of(v)
        chain = [x for x in _chain_at(g.file_chain.get(rel, ([], set(), []))[0], a) if x]
        cands = g.resolve_defs(qual, name, rel, chain)
        if not qual.strip() and not cands:
            lam = self._local_lambda(src, a, name)
            if lam is not None:
                return self.pred_body(src, lam, depth + 1)
        vals = []
        for d in cands:
            if FIX["D6"]:
                vals.append(self.pred_body(self.ctx.src(d["path"]), d["ob"], depth + 1))
            elif d["cls"]:
                v = (CLS_LVL[d["cls"]], d["env"], False)
                vals.append((v, _neg_of(v)))
            else:
                vals.append((V_NONE, V_NONE))
        if not vals:
            return None
        return min(vals, key=lambda pn: _rank(pn[0]))   # ambiguous -> the weakest

    def _local_lambda(self, src, a, name):
        c = src.clean
        fb = self.func_block(src, a)
        lo = fb if fb >= 0 else 0
        best = None
        for m in re.finditer(r"\b(?:auto|const\s+auto)\s+" + re.escape(name) + r"\s*=\s*\[", c[lo:a]):
            best = lo + m.end() - 1
        if best is None:
            return None
        ob = c.find("{", best)
        return ob if ob in src.brace_match and ";" not in c[best:ob] else None

    def pred_body(self, src, ob, depth=0):
        """(when-true, when-false) of a bool function / lambda body at ob: the
        weakest of its returns, each ANDed with the guards in front of it."""
        key = (src.path, ob)
        if key in self._pred:
            return self._pred[key]
        self._pred[key] = (V_NONE, V_NONE)              # recursion guard
        c = src.clean
        cb = src.brace_match.get(ob)
        vals = []
        if cb is not None and depth < 12:
            for m in re.finditer(r"\breturn\b", c[ob:cb]):
                r = ob + m.start()
                inner = src.enclosing(r)
                nested = inner[:inner.index(ob)] if ob in inner else []
                if any(self._is_lambda_block(src, x) for x in nested):
                    continue                            # a nested lambda's return
                e = self._stmt_end(src, r + 6, cb)
                val = self.ev(src, r + 6, e, depth + 1)[0]
                if e <= r + 6:
                    continue
                vals.append(_v_max([self.value_at(src, r, stop_ob=ob), val]))
        pos = min(vals, key=_rank) if vals else V_NONE
        # a NAMED predicate that is true by default is a default-ON switch
        # (`return !e || e[0] != '0'`): using it positively is a real gate
        pos = (pos[0], pos[1], False)
        res = (pos, _neg_of(pos))
        self._pred[key] = res
        return res

    def var_value(self, src, a, name, depth):
        """(when-true, when-false) of a bool variable / constant used at a."""
        c = src.clean
        fb = self.func_block(src, a)
        decl_re = re.compile(r"\b(?:(?:static|const|constexpr|inline|thread_local)\s+)*(?:bool|auto)\s+"
                             + re.escape(name) + r"\s*(=|\{|\()")
        found, scope = None, None
        # (round 4: an index of bool/auto declarations per file - the whole-file
        # scan per atom dominated the run once every occurrence became a site)
        idx = self._decl_index.get(src.path)
        if idx is None:
            idx = self._decl_index[src.path] = {}
            for m in re.finditer(r"\b(?:(?:static|const|constexpr|inline|thread_local)\s+)*(?:bool|auto)\s+"
                                 r"(\w+)\s*(?:=|\{|\()", c):
                idx.setdefault(m.group(1), []).append(m.start())
        cands = [m for m in (decl_re.match(c, p) for p in idx.get(name, [])) if m]
        if fb >= 0:
            for m in cands:
                if not (fb <= m.start() < a):
                    continue
                blk = src.innermost(m.start())
                if blk == -1 or blk in src.enclosing(a):
                    found = m                       # in scope at a (nearest wins)
            scope = (fb, src.brace_match.get(fb, len(c)))
        if found is None:
            for m in cands:
                if self.func_block(src, m.start()) == -1:
                    found, scope = m, (0, len(c))
                    break
        if found is None:
            return None
        key = (src.path, found.start())
        if key in self._var:
            return self._var[key]
        self._var[key] = (V_NONE, V_NONE)
        lo, hi = scope
        assigns = [m for m in re.finditer(r"(?<![\w.>])" + re.escape(name) +
                                          r"\s*(?:=(?!=)|\|=|&=|\^=)", c[lo:hi])
                   if lo + m.start() < found.start() or lo + m.start() > found.end()]
        if re.search(r"(?<![&\w)\]])&\s*" + re.escape(name) + r"\b", c[lo:hi]):
            return None                                  # address taken: unknown
        if FIX["D13"]:
            # D13: a global with external linkage that ANOTHER file assigns, and
            # a flag handed to a callee by non-const reference, are not their
            # initializer's constant (verifier p4 Q1 / Q2: both read "dead")
            if scope == (0, len(c)) and self._written_elsewhere(src, name, found):
                return None
            if self._passed_by_ref(src, name, lo, hi):
                return None
        if assigns:
            # a FUNCTION-LOCAL flag (`bool framed = false; if (gate()) { ..
            # framed = true; } .. if (framed) write`): true only where some
            # assignment made it true, each under the guards in front of it.
            # Only the positive use is known (`!framed` says nothing about any
            # switch). Globals may be assigned from other files: unknown.
            if fb < 0 or scope[0] != fb or not FIX["D6"]:
                return None
            if not re.match(r"\s*static\b", c[found.start():found.start() + 7]) and \
                    "static" not in c[max(fb, found.start() - 30):found.start()].split(";")[-1]:
                # a plain local: flow-sensitive (the definition that reaches a)
                v = self._flow_value(src, name, found, a, depth)
                if v is not None:
                    self._var.pop(key, None)            # position-dependent: not cached
                    return (v, V_NONE)
            init = self._init_value(src, found, depth)
            if init is None:
                return None
            vals = [init[0]]
            for m in assigns:
                if not m.group(0).rstrip().endswith("=") or re.search(r"[|&^]=\s*$", m.group(0)):
                    return None                          # compound: unknown
                ea = lo + m.end()
                eb = self._stmt_end(src, ea)
                val = self.ev(src, ea, eb, depth + 1)[0]
                vals.append(_v_max([self.value_at(src, lo + m.start(), stop_ob=fb), val]))
            res = (min(vals, key=_rank), V_NONE)
        else:
            res = self._init_value(src, found, depth)
            if res is None:
                return None
        self._var[key] = res
        return res

    def _called_in_tree(self, fname):
        """fname is referenced (outside its own definition / a prototype) in the
        scanned production tree."""
        for path, idx, clean in self.ctx.refs(fname):
            if _proto_re(fname).match(clean):
                continue
            _lines, funcs = self.ctx.by_path[path]
            enc = enclosing_func(funcs, idx)
            if enc is None and _def_head(funcs, idx, fname):
                continue
            if enc is not None and enc[2] == fname:
                continue
            return True
        return False

    _BYREF_STD = {"swap", "exchange", "tie", "getline", "sscanf", "scanf", "fscanf", "sscanf_s",
                  "iter_swap", "read", "fread"}

    def _written_elsewhere(self, src, name, found):
        c = src.clean
        head = c[max(0, found.start() - 40):found.start()]
        if re.match(r"\s*static\b", c[found.start():found.end()]) or re.search(r"\bstatic\s+(?:\w+\s+)*$", head.split(";")[-1].split("}")[-1]) or \
                "" in _chain_at(_ns_spans(c), found.start()):
            return False                                 # internal linkage
        wr = re.compile(r"(?<![\w.>])" + re.escape(name) + r"\s*(?:=(?!=)|\|=|&=|\^=|\+\+|--|\.\s*(?:store|exchange|fetch_\w+)\s*\()"
                        r"|(?:\+\+|--)\s*" + re.escape(name) + r"\b|(?<![&\w)\]])&\s*" + re.escape(name) + r"\b")
        for path, _idx, clean in self.ctx.refs(name):
            if path != src.path and wr.search(clean):
                return True
        return False

    def _passed_by_ref(self, src, name, lo, hi):
        """`name` is a bare call argument whose parameter is a non-const reference
        (resolved through the tree's definitions; std by-ref helpers too)."""
        c = src.clean
        for m in re.finditer(r"[(,]\s*(" + re.escape(name) + r")\s*(?=[,)])", c[lo:hi]):
            q = lo + m.start(1)
            depth, i = 0, q - 1
            while i >= 0:
                if c[i] in ")]}":
                    depth += 1
                elif c[i] in "([{":
                    if depth == 0:
                        break
                    depth -= 1
                i -= 1
            if i < 0 or c[i] != "(":
                continue
            kind, callee, _s = _owner(c, i)
            if kind != "call":
                continue
            span = _arg_span(c, i, q)
            if span is None:
                continue
            idx = span[0]
            if callee in self._BYREF_STD:
                return True
            for sig in self.ctx.sigs_named(callee):
                ps = _params_of(sig)
                if idx < len(ps):
                    ty = ps[idx][1]
                    if "&" in ty and "&&" not in ty and not re.search(r"\bconst\b", ty):
                        return True
        return False

    def _flow_value(self, src, name, found, a, depth):
        """When-true value of the local bool `name` at a: walk the statements
        in front of a (innermost block outward) back to the definition that
        reaches it. Understood: `V = E;`, `if (C) <..V = E..>` (V may become
        true: V or (C and E)), `if (V && R) { V = false; }` (V and !R). Anything
        else touching V -> None (the caller falls back to the flow-insensitive
        reading)."""
        c = src.clean
        asg = re.compile(r"(?<![\w.>])" + re.escape(name) + r"\s*=(?!=)")
        chain = src.enclosing(a)
        fb = self.func_block(src, a)
        pending = []                        # statements (latest first) after the definition
        for ob in chain:
            if ob > found.start():
                # a loop body between the definition and a that assigns V: a
                # later statement of the body reaches a on the next iteration
                st, _ = self._stmt_start(src, ob, src.parent.get(ob, -1))
                if re.match(r"\s*(?:for|while|do)\b", c[st:ob]) and asg.search(c, ob, src.brace_match.get(ob, ob)):
                    return None
            for s, e in reversed(self.top_stmts(src, ob)):
                if s >= a or (s <= a <= e):
                    continue
                if s <= found.start() <= e:
                    base = self._init_value(src, found, depth)
                    if base is None:
                        return None
                    return self._apply_flow(src, name, base[0], pending, depth)
                if asg.search(c, s, e + 1):
                    m = re.compile(r"\s*" + re.escape(name) + r"\s*=(?!=)").match(c, s)
                    if m:                           # V = E; at this level: reaches a
                        return self._apply_flow(
                            src, name, self.ev(src, m.end(), self._stmt_end(src, m.end()), depth + 1)[0],
                            pending, depth)
                    pending.append((s, e))
            if ob == fb:
                break
        return None

    def _apply_flow(self, src, name, val, stmts, depth):
        c = src.clean
        for s, e in reversed(stmts):               # oldest first
            g = self._cond_of_if(src, _ws(c, s))
            if not g:
                return None
            t_end, _x = self._branch(src, g[2] + 1, e + 1)
            k = _ws(c, t_end + 1)
            if c.startswith("else", k) and not _ident(c, k + 4):
                return None
            j = _ws(c, g[2] + 1)
            body = (j + 1, t_end) if c[j] == "{" else (j, t_end + 1)
            sets = list(re.finditer(r"(?<![\w.>])" + re.escape(name) + r"\s*=(?!=)\s*([^;]*);", c[body[0]:body[1]]))
            if not sets:
                return None
            if all(re.fullmatch(r"\s*(?:false|0)\s*", m.group(1)) for m in sets):
                # V = false under C: V survives only where C fails - provided the
                # reset is UNCONDITIONAL inside the branch
                reset = re.compile(r"\s*" + re.escape(name) + r"\s*=\s*(?:false|0)\s*;")
                if c[j] == "{":
                    if not any(reset.match(c, x) for x, _y in self.top_stmts(src, j)):
                        return None
                elif not reset.match(c, j):
                    return None
                conj = _split_top(c, g[0], g[1], "&&")
                rest = [(x, y) for x, y in conj if c[x:y].strip() != name]
                if len(rest) == len(conj):
                    val = _v_max([val, self.ev(src, g[0], g[1], depth + 1)[1]])
                elif rest:
                    val = _v_max([val, _v_or([self.ev(src, x, y, depth + 1)[1] for x, y in rest])])
                else:
                    val = (DEAD, "", False)
                continue
            # V = E (true-ish) somewhere under C: V or (C and guards and E)
            alts = []
            for m in sets:
                q = body[0] + m.start()
                ev_ = self.ev(src, body[0] + m.start(1), body[0] + m.end(1), depth + 1)[0]
                inner = self.value_at(src, q, stop_ob=j) if c[j] == "{" else V_NONE
                alts.append(_v_max([self.ev(src, g[0], g[1], depth + 1)[0], inner, ev_]))
            val = _v_or([val] + alts) if FIX["D5"] else min([val] + alts, key=_rank)
        return val

    def _init_value(self, src, found, depth):
        """(when-true, when-false) of the initializer of the declaration `found`."""
        c = src.clean
        if found.group(1) == "=":
            ia = found.end()
            ib = self._stmt_end(src, ia)
        else:
            op = found.end() - 1
            ia = op + 1
            ib = src.brace_match.get(op) if c[op] == "{" else src._match_paren(op)
            if ib is None:
                return None
        ia = _ws(c, ia)
        if ia < ib and c[ia] == "[":                     # [..](..) { .. }() lambda
            lob = c.find("{", ia, ib)
            lcb = src.brace_match.get(lob)
            if lob < 0 or lcb is None or not re.match(r"\s*\(\s*\)", c[lcb + 1:ib]):
                return (V_NONE, V_NONE)
            return self.pred_body(src, lob, depth + 1)
        return self.ev(src, ia, ib, depth + 1)

def _find_top_op(c, a, b, op):
    depth = 0
    for i in range(a, b - 1):
        ch = c[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif depth == 0 and c.startswith(op, i) and (i == 0 or c[i - 1] not in "<>=!"):
            return i
    return -1


def _open_paren(c, close):
    depth = 0
    for i in range(close, -1, -1):
        if c[i] == ")":
            depth += 1
        elif c[i] == "(":
            depth -= 1
            if depth == 0:
                return i
    return None


# Kinds that write guest memory: inside an export (the guest's own call) they
# fill its out-params / objects.
EXPORT_WRITE_KINDS = ("guest_write", "raw_store", "guest_memset", "guest_memcpy")
EXPORT_THREAD = "export-thread: "


def classify(ctx, path, lines, funcs, line_idx, kind="guest_write", at=None, entry=False, pos=None):
    """(cls, detail, enclosing function) of the site on line line_idx.
    `at` = the identifier whose occurrence on the line is the site (a caller
    reference); default = the primitive itself. D12: `pos` = the exact
    occurrence (clean-text offset) - every occurrence is judged on its own."""
    if FIX["D2"]:
        return classify_site(ctx, path, lines, funcs, line_idx, kind, at, entry, pos)
    return classify_legacy(ctx, path, lines, funcs, line_idx, kind)


def _site_pos(ctx, path, line_idx, kind, at, pos=None):
    s = ctx.src(path)
    if pos is not None and FIX["D12"]:
        return s, pos
    ls = s._line_starts[line_idx]
    le = s._line_starts[line_idx + 1] if line_idx + 1 < len(s._line_starts) else len(s.clean)
    line = s.clean[ls:le]
    rx = re.compile(r"\b" + re.escape(at) + r"\b") if at else ctx.prim_rx.get(kind)
    m = rx.search(line) if rx else None
    if m is None:
        m = re.search(r"\S", line)
    return s, ls + (m.start() if m else 0)


def classify_site(ctx, path, lines, funcs, line_idx, kind, at, entry=False, pos=None):
    """D2: the gate AT THE SITE decides (SiteGates.level_at); MASTER, opt-in and
    dead code are final. Only a site that runs by default (no gate, or a
    default-ON own gate) can then be an export's own out-param write (D3) or a
    reviewed allowlisted platform fix."""
    rel = ctx.rel(path)
    fname = "(file scope)"
    enc = enclosing_func(funcs, line_idx)
    body = ""
    if enc:
        fname = enc[2]
        body = "\n".join(lines[enc[0]:enc[1] + 1])
    # R1 (docs/SPLICE_CONVERSION.md): kernel/r1_hooks_*.cpp holds the host side of the hooks
    # on generated code (tools/regen/superman_regen.toml [[midasm_hook]]) and the R1 ports. A
    # write primitive there is one part of a HOOK, and hooks are judged as hooks: every one is
    # in docs/OVERRIDES.json with a class and `default_exe`, and the `live` / `conditional`
    # band-aids are COUNTED in the forcing number (hooks_report, --count). So the site gets
    # class HOOK here instead of being counted a second time as UNGATED.
    if R1_PORT_SUBSTR in rel.replace("\\", "/"):
        return "HOOK", "inside a registered hook / R1 port (counted per hook: see HOOKS)", fname
    lam = ctx.export_lambda_at(path, line_idx) if FIX["B6"] else None
    if lam:
        body = "\n".join(lines[lam[0]:lam[1] + 1])
    src, pos = _site_pos(ctx, path, line_idx, kind, at, pos)
    exp = lam[2] if lam else ctx.export_of(fname)
    how = "" if (lam or fname in ctx.exports) else f" (helper {fname} called only from exports)"
    thread = ctx.launch_lambda_at(path, pos) if exp else None
    if thread and FIX["D3"]:
        # a thread / timer the export STARTS runs on its own, after the call
        # returns: not the export's out-param contract (verifier P6)
        lo = ctx.src(path)._line_starts[lam[0]] if lam else ctx.src(path)._line_starts[enc[0]] if enc else 0
        if thread[0] < lo:
            thread = None                   # the export is inside the launch, not vice versa
    if not FIX["D3"] and exp:
        r = _export_allow(exp, kind, how)   # D3 reverted: the export rule ran first
        if r:
            return r[0], r[1], fname
    lvl, env, conds = ctx.gates_site.level_at(src, pos, inherited=at is not None)
    if lvl == DEAD:
        return "OWNGATE", "observe-only(dead)", fname
    if lvl == MASTER:
        return "MASTER", "", fname
    if lvl == DEVTOOL:
        return "DEVTOOL", env, fname
    if lvl == OFF:
        return "OWNGATE", env, fname
    # runs by default (NONE) or behind a default-ON own gate (ON)
    if entry and FIX["D3"]:
        # a thread / callback ENTRY reference (std::thread(fn), a timer): the
        # launched code runs on its own, so neither the launcher's export
        # contract nor its allowlist entry covers it - only its gate does
        return ("OWNGATE_ON", env, fname) if lvl == ON else ("UNGATED", "", fname)
    if FIX["D14"]:
        # D14: a site whose own condition ORs the master gate with anything
        # (`if (own() || forcingAllowed())`) says in code that it is forcing -
        # neither the export rule nor the allowlist may turn it into ALLOW
        if [c for c in conds if not c.startswith("!") and or_bypass(c)]:
            return ("OWNGATE_ON" if lvl == ON else "UNGATED",
                    "OR-bypass of the master gate" + (f" ({env})" if env else ""), fname)
    near = "\n".join(lines[max(0, line_idx - 5):line_idx + 6])
    site_line = lines[line_idx]
    for fsub, anchor, reason in ALLOWLIST:
        if fsub in rel and allow_anchor_hit(anchor, fname, site_line, near):
            return "ALLOW", reason, fname
    for fsub, func, row in DECLARED_BANDAIDS:
        if fsub in rel and fname == func:
            if row in _bandaids_text():
                return "OWNGATE_ON", f"DECLARED BAND-AID, ungated (docs/BANDAIDS.md: {row})", fname
            return "UNGATED", f"declared band-aid row '{row}' MISSING from docs/BANDAIDS.md", fname
    if exp:
        if thread and FIX["D3"]:
            return ("UNGATED" if lvl == NONE else "OWNGATE_ON",
                    EXPORT_THREAD + f"in a thread export {exp} launches" + (f" ({env})" if env else ""), fname)
        r = _export_allow(exp, kind, how)
        if r:
            return r[0], r[1], fname
    if not FIX["D6"] and lvl == NONE:
        m = re.search(r'getenv\(\s*"(SR_[A-Z0-9_]+)"', body)
        if m:
            return "OWNGATE", m.group(1), fname          # D6 reverted: first getenv
    if lvl == ON:
        if any("forcingAllowed" in c for c in conds):
            return "OWNGATE_ON", f"OR-bypass of the master gate ({env})", fname
        return "OWNGATE_ON", env, fname
    return "UNGATED", "", fname


def allow_anchor_hit(anchor, fname, site_line, near):
    """ALLOWLIST anchor forms: "" (the whole file), "fn:NAME" (the enclosing
    function), "line:CODE" (CODE on the site line itself, comments stripped -
    round 4: the precise form; a neighbour or a comment can never match), or
    plain CODE (on the site line or within +-5 lines; legacy)."""
    if anchor == "":
        return True
    if anchor.startswith("fn:"):
        return anchor[3:] == fname
    if anchor.startswith("line:"):
        return anchor[5:] in site_line.split("//")[0]
    return anchor in site_line or anchor in near


def _export_allow(exp, kind, how):
    if kind in (EXPORT_WRITE_KINDS if FIX["D7"] else ("guest_write",)):
        return "ALLOW", f"export {exp} fills its own out-params/object{how}"
    if kind == "host_sync" and EXPORT_SIGNAL_RE.search(exp):
        return "ALLOW", f"export {exp}: the guest's own signal{how}"
    return None


def classify_legacy(ctx, path, lines, funcs, line_idx, kind="guest_write"):
    """The whole-function classifier (D2 reverted), kept for the mutation test."""
    rel = ctx.rel(path)
    body, fname, fstart = "", "(file scope)", line_idx
    enc = enclosing_func(funcs, line_idx)
    if enc:
        fstart = enc[0]
        body = "\n".join(lines[enc[0]:enc[1] + 1])
        fname = enc[2]
    # B6: a lambda handed straight to registerExport IS that export; its body,
    # not the registering function's (registerStubs), is the code that runs.
    lam = ctx.export_lambda_at(path, line_idx) if FIX["B6"] else None
    if lam:
        body, fstart = "\n".join(lines[lam[0]:lam[1] + 1]), lam[0]
    site_line = lines[line_idx]
    # iter-377 (triage step 0a): match the allowlist anchor against the site line
    # or a ±5-line WINDOW, never the whole enclosing function body. The body match
    # was a verified defect: guest_context.cpp's fake-mstate forcing classified as
    # ALLOW because an EDRAM anchor appeared hundreds of lines away in the same
    # (huge) function — the exact site the audit exists to catch was invisible.
    # An anchor "fn:NAME" matches the enclosing function by name instead.
    near = "\n".join(lines[max(0, line_idx - 5):line_idx + 6])
    for fsub, anchor, reason in ALLOWLIST:
        if fsub in rel and (anchor == "" or (anchor[3:] == fname if anchor.startswith("fn:")
                                             else (anchor in site_line or anchor in near))):
            return "ALLOW", reason, fname
    for fsub, func, row in DECLARED_BANDAIDS:
        if fsub in rel and fname == func:
            if row in _bandaids_text():
                return "OWNGATE_ON", f"DECLARED BAND-AID, ungated (docs/BANDAIDS.md: {row})", fname
            return "UNGATED", f"declared band-aid row '{row}' MISSING from docs/BANDAIDS.md", fname
    # iter-463: the master gate OR'd with another operand runs without it.
    if or_bypass(body):
        m = re.search(r'getenv\(\s*"(SR_[A-Z0-9_]+)"', body)
        return "OWNGATE_ON", "OR-bypass of the master gate" + (f" ({m.group(1)})" if m else ""), fname
    # iter-463: a registered kernel export implementing the guest's own call
    # (B6: an export lambda; B7: a helper only exports call).
    exp = lam[2] if lam else ctx.export_of(fname)
    if exp:
        how = "" if (lam or fname in ctx.exports) else f" (helper {fname} called only from exports)"
        if kind == "guest_write":
            return "ALLOW", f"export {exp} fills its own out-params/object{how}", fname
        if kind == "host_sync" and EXPORT_SIGNAL_RE.search(exp):
            return "ALLOW", f"export {exp}: the guest's own signal{how}", fname
    # observe-only / dead-code guard: a write gated by a compile/observe flag that
    # is off means it never runs -> treat as gated (documented dead code).
    if re.search(r"kObserveOnly|if\s*\(\s*0\s*\)", body):
        return "OWNGATE", "observe-only(dead)", fname
    # gate detection: enclosing function calls forcingAllowed() OR any gate predicate
    # (B1: resolved per definition; B2: the master gate wins over an own-gate).
    g = ctx.gates.body_gate(body, rel, ctx.gates.chain_of(rel, fstart))
    if g:
        return g[0], ("" if g[0] == "MASTER" else g[1]), fname
    m = re.search(r'getenv\(\s*"(SR_[A-Z0-9_]+)"', body)
    if m:
        return "OWNGATE", m.group(1), fname
    return "UNGATED", "", fname


# iter-455l r4 (director): the named-isolation choke point. Under
# SR_FORCING_ISOLATE=<name>, every plain forcingAllowed() caller is denied by the
# ONE implementation in kernel/guest_context.cpp and only forcingAllowedFor(<name>)
# with the byte-exact name is authorizable. That suppression is semantic, so what
# the audit must establish mechanically is that the choke point cannot be bypassed:
#   1. the master/isolation envs are read NOWHERE outside guest_context.cpp;
#   2. every authorization goes through forcingAllowed() or forcingAllowedFor();
#   3. forcingAllowedFor is called only with STRING LITERALS drawn from the
#      reviewed exception set below (a non-literal argument is unauditable);
#   4. generated guest/ppc contains no reference to either env or the named API.
# Growing ISOLATION_EXCEPTIONS is a reviewed edit, exactly like ALLOWLIST.
# iter-455l retirement: SR_H7C_NOREBASE deleted per its retirement contract (director
# D1 adjudication). The set is EMPTY until the next named diagnostic gate is reviewed
# in; any forcingAllowedFor caller with an unlisted name fails this audit.
# iter-463 burndown round 2: SR_REAL_HEAP_ZERO (guest/ppc_import_glue.cpp
# sub_8246CA80) - the malloc zero-fill band-aid, master-gated with a NAMED gate
# so the one behavior change it guards can be rolled alone:
#   SR_ALLOW_FORCING=1 SR_FORCING_ISOLATE=SR_REAL_HEAP_ZERO
# ... and SR_SCHED_THROTTLE (kernel/cs_timeout_underflow_fix.cpp sub_826F4230 /
# sub_826F4378): the resource-scheduler empty-queue sleep, a timing band-aid
# that fired in 68/69 recent run logs - rolled alone with
#   SR_ALLOW_FORCING=1 SR_FORCING_ISOLATE=SR_SCHED_THROTTLE
# ... and SR_CFG_SOUND_OFF (kernel/fe_flow_watch.cpp cfgSoundOffThread launch): the Sound-flag
# zeroing, rolled alone to keep the audio subsystem off while every other burndown item stays
# natural (open defect D-AUDIO-CMDBUF, docs/NATURAL_BASELINE.md 2026-10-01):
#   SR_ALLOW_FORCING=1 SR_FORCING_ISOLATE=SR_CFG_SOUND_OFF
# ... and SR_DIAG_NULLPAGE_POISON (kernel/audio_cmdq_obs.cpp poisonMain launch): the S2
# D-AUDIO-CMDBUF causal diagnostic (0xFF over guest [0, N) every 100 ms). It must roll ALONE:
# plain SR_ALLOW_FORCING=1 would also start cfgSoundOffThread and mute the audio path under test.
#   SR_ALLOW_FORCING=1 SR_FORCING_ISOLATE=SR_DIAG_NULLPAGE_POISON
ISOLATION_EXCEPTIONS = {"SR_REAL_HEAP_ZERO", "SR_SCHED_THROTTLE", "SR_CFG_SOUND_OFF",
                        "SR_DIAG_NULLPAGE_POISON"}
CHOKE_FILE = os.path.join("kernel", "guest_context.cpp")

def isolation_report(parsed, quiet=False):
    env_re = re.compile(r'getenv\s*\(\s*"SR_(?:ALLOW_FORCING|FORCING_ISOLATE)"')
    plain_re = re.compile(r'\bforcingAllowed\s*\(\s*\)')
    named_re = re.compile(r'\bforcingAllowedFor\s*\(\s*(?:("([^"]*)")|([^)]*))\)')
    violations = []
    plain_sites, named_sites = [], []
    for path, lines, _funcs in parsed:
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        is_choke = os.path.normpath(path).endswith(os.path.normpath(CHOKE_FILE))
        in_block = False
        for idx, line in enumerate(lines):
            clean, in_block = strip_for_braces(line, in_block)
            # strip_for_braces removes STRING LITERALS too, so the env names and the
            # forcingAllowedFor("...") literal never survive in `clean`. Match against
            # the //-comment-stripped RAW line, but only where the identifier survives
            # comment stripping (so commented-out code cannot flag or hide anything).
            code = line.split("//")[0]
            if "getenv" in clean and env_re.search(code) and not is_choke:
                violations.append(f"{rel}:{idx+1} reads a master/isolation env "
                                  f"DIRECTLY (bypasses the choke point)")
            if is_choke:
                continue                       # the implementation itself
            if plain_re.search(clean):
                plain_sites.append((rel, idx + 1))
            if "forcingAllowedFor" not in clean:
                continue
            for m in named_re.finditer(code):
                if m.group(2) is None:
                    violations.append(f"{rel}:{idx+1} forcingAllowedFor with a "
                                      f"NON-LITERAL argument (unauditable)")
                elif m.group(2) not in ISOLATION_EXCEPTIONS:
                    violations.append(f"{rel}:{idx+1} forcingAllowedFor(\"{m.group(2)}\") "
                                      f"not in the reviewed exception set")
                else:
                    named_sites.append((rel, idx + 1, m.group(2)))
    # generated guest code (guest/ppc, gitignored hand-edit surface): PLAIN callers are
    # COUNTED — they reach the same forcingAllowed() choke point and are suppressed
    # under isolation, so excluding them made the old "all callers enumerated" claim
    # false (iter-455l r4 verdict: ppc_recomp.2.cpp carried a live plain caller the
    # report never saw). NAMED authorization or direct env reads in generated code are
    # VIOLATIONS. Cheap two-phase: substring prefilter on the whole text, then the
    # same comment-aware per-line matching as the parsed pass, candidates only.
    gen_plain = []
    gen_plain_re = re.compile(r'\bsr::kernel::forcingAllowed\s*\(\s*\)')
    gen_env_re = re.compile(r'getenv\s*\(\s*"SR_(?:ALLOW_FORCING|FORCING_ISOLATE)"')
    gen_dir = os.path.join(ROOT, GENERATED_DIR)
    if os.path.isdir(gen_dir):
        for root, _, files in os.walk(gen_dir):
            for fn in files:
                if not fn.endswith((".cpp", ".h", ".hpp", ".cc")):
                    continue
                p = os.path.join(root, fn)
                try:
                    text = open(p, encoding="utf-8", errors="replace").read()
                except OSError:
                    continue
                if ("forcingAllowed" not in text and "SR_ALLOW_FORCING" not in text
                        and "SR_FORCING_ISOLATE" not in text):
                    continue
                rel = os.path.relpath(p, ROOT).replace("\\", "/")
                in_block = False
                for idx, line in enumerate(text.split("\n")):
                    clean, in_block = strip_for_braces(line, in_block)
                    code = line.split("//")[0]
                    if "getenv" in clean and gen_env_re.search(code):
                        violations.append(f"{rel}:{idx+1} GENERATED code reads a "
                                          f"master/isolation env directly")
                    if "forcingAllowedFor" in clean:
                        violations.append(f"{rel}:{idx+1} GENERATED code uses the "
                                          f"named-auth API (only the kernel H7C gate may)")
                        continue
                    if gen_plain_re.search(clean):
                        gen_plain.append((rel, idx + 1))
    if not quiet:
        print()
        print("--- ISOLATION REPORT (SR_FORCING_ISOLATE choke point) ---")
        print(f"  plain forcingAllowed() callers: {len(plain_sites)} parsed "
              f"(kernel/gfx/app/guest) + {len(gen_plain)} generated (guest/ppc) = "
              f"{len(plain_sites) + len(gen_plain)}, ALL suppressed under isolation")
        for rel, ln in gen_plain:
            print(f"    generated: {rel}:{ln}")
        print(f"  named forcingAllowedFor callers: {len(named_sites)} "
              f"{sorted(set(n for _, _, n in named_sites))} "
              f"(reviewed exception set: {sorted(ISOLATION_EXCEPTIONS)})")
        for rel, ln, name in named_sites:
            print(f"    {rel}:{ln}  forcingAllowedFor(\"{name}\")")
        if violations:
            print(f"  VIOLATIONS ({len(violations)}):")
            for v in violations:
                print(f"    {v}")
        else:
            print("  no bypass reads, no non-literal names, no generated named-auth "
                  "or env references: under SR_FORCING_ISOLATE=<name> every plain "
                  "caller above — parsed AND generated — is suppressed by "
                  "construction and only the named exception is authorizable.")
    return violations


# ============================================================== DEVTOOL ====
# iter-463 phase 2 (docs/DEVTOOLS_DESIGN.md 2.1): the hard rules of the dev-tool class.
DEVTOOL_FILE = "kernel/devtools.cpp"
DEVTOOLS_JSON = os.path.join("docs", "DEVTOOLS.json")
_DEVTOOL_PRED_RE = re.compile(r"\b(devToolAllowed|devToolHookNeeded_\w+)\s*\(")
_DEVTOOL_ENV_RE = re.compile(r'getenv\s*\(\s*"(SR_DEVTOOLS\w*)"')


def is_devtool_pred(name):
    return name == "devToolAllowed" or name.startswith("devToolHookNeeded_")


def devtool_closed_set(root):
    """[(name, class)] of kernel/devtools.cpp's kTools[], or None when the tree has none."""
    p = os.path.join(root, DEVTOOL_FILE)
    if not os.path.isfile(p):
        return None
    with open(p, encoding="utf-8", errors="replace") as f:
        text = f.read()
    m = re.search(r"kTools\[\]\s*=\s*\{(.*?)\};", text, re.S)
    return re.findall(r'\{\s*"(\w+)"\s*,\s*"(\w+)"\s*\}', m.group(1)) if m else []


def _paren_close(c, o):
    depth = 0
    for i in range(o, len(c)):
        if c[i] == "(":
            depth += 1
        elif c[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


# The dev-tool gate is default OFF by construction (DEVTOOLS_DESIGN 1.2): a tool turns on ONLY from
# SR_DEVTOOLS or dev.txt, parsed in devToolsCompute(). The `!e ||` idiom check alone missed every
# realistic default-ON edit (iter-463 review: 5/5 mutants passed - a pushed tool name, a seeded
# mask, `return true ||` in devToolAllowed, an env fallback literal). These rules close those
# shapes structurally; tools/difftest/devtools_test.cpp D0/D1a is the behavioural backstop.
_DEVTOOL_TABLES = ("kTools", "kTickHookTools")
_DEVTOOL_PRED_DEF_RE = re.compile(
    r"\b(?:bool|uint32_t)\s+(devToolAllowed|devToolMask|devToolHookNeeded_\w+|devToolsAnyDevtoolClass)"
    r"\s*\([^)]*\)\s*\{")
_DEVTOOL_SEAM_RE = re.compile(r"\b(devToolsResetForTest|devToolsSetTaintForTest)\s*\(")
_DEVTOOL_LIT = r"(0[xX][0-9a-fA-F]+|\d+)[uUlL]*(?=\s*[;,)}])"     # a whole literal operand


def _devtool_default_off(rel, raw, clean, names):
    """-> [violation] for kernel/devtools.cpp: no tool can be on without SR_DEVTOOLS / dev.txt."""
    v = []

    def at(pos):
        return raw.count("\n", 0, pos) + 1

    spans = []
    for t in _DEVTOOL_TABLES:
        m = re.search(r"\b%s\s*\[\s*\]\s*=\s*\{" % t, clean)
        if m:
            spans.append((m.start(), _brace_close(clean, m.end() - 1) or len(clean)))
    lower = {n.lower() for n in names}
    # string literals: quotes survive strip_code (contents are blanked), so pairs in `clean` are
    # exactly the literals; their text is read back from `raw`
    for m in re.finditer(r'"[^"\n]*"', clean):
        text = raw[m.start() + 1:m.end() - 1].strip().lower()
        if text in lower and not any(a <= m.start() <= b for a, b in spans):
            v.append(f"{rel}:{at(m.start())} names tool {text!r} outside {'/'.join(_DEVTOOL_TABLES)}[] "
                     "(a tool turns on only from SR_DEVTOOLS / dev.txt)")
    for m in re.finditer(r"(?<!sizeof\()\bkTools\s*\[\s*\d", clean):
        v.append(f"{rel}:{at(m.start())} indexes kTools[] by a literal (a hard-coded tool)")
    for m in re.finditer(r"\b1[uUlL]*\s*<<\s*\d", clean):
        v.append(f"{rel}:{at(m.start())} a hard-coded tool bit (`1 << <literal>`)")
    for m in re.finditer(r"\b(?:g_mask|mask|seen)\s*(?:\|=|=|\{)\s*" + _DEVTOOL_LIT, clean):
        if int(m.group(1), 0) != 0:
            v.append(f"{rel}:{at(m.start())} a mask seeded with a non-zero literal")
    for m in re.finditer(r"\b(?:g_mask\s*\.\s*(?:store|fetch_or|exchange)|applyMask)\s*\(\s*" + _DEVTOOL_LIT, clean):
        if int(m.group(1), 0) != 0:
            v.append(f"{rel}:{at(m.start())} the mask set to a non-zero literal")
    for m in re.finditer(r"\bg_hook\s*(?:\.\s*store\s*\(|\{|=)\s*true\b", clean):
        v.append(f"{rel}:{at(m.start())} the tick hooks forced on (g_hook := true)")
    for m in _DEVTOOL_PRED_DEF_RE.finditer(clean):
        cb = _brace_close(clean, m.end() - 1)
        body = clean[m.end():cb] if cb else ""
        if re.search(r"\btrue\b|\|\||\breturn\s+[1-9]", body):
            v.append(f"{rel}:{at(m.start())} {m.group(1)}() can answer yes without the mask "
                     "(`true` / `||` / a literal in the predicate body)")
    return v


def devtool_report(parsed, root, sites, quiet=False, out=sys.stdout):
    """-> [violation]. Empty for a tree without kernel/devtools.cpp."""
    cset = devtool_closed_set(root)
    if cset is None:
        return []
    v = []
    names, cls_of = {n for n, _c in cset}, dict(cset)
    if not cset:
        v.append(f"{DEVTOOL_FILE}: no kTools[] closed set found")
    for n, c in cset:
        if c not in ("observe", "devtool"):
            v.append(f"{DEVTOOL_FILE}: tool {n!r} has class {c!r} (observe | devtool)")
        if n == "all":
            v.append(f"{DEVTOOL_FILE}: 'all' is not a tool (design 1.1)")
    jp = os.path.join(root, DEVTOOLS_JSON)
    try:
        with open(jp, encoding="utf-8") as f:
            rows = json.load(f).get("tools", [])
        jset = {(r.get("name"), r.get("class")) for r in rows}
        if jset != set(cset):
            v.append(f"closed set mismatch: {DEVTOOL_FILE} {sorted(cset)} != {DEVTOOLS_JSON} {sorted(jset)}")
        for r in rows:
            if not r.get("gate") or not r.get("protocol"):
                v.append(f"{DEVTOOLS_JSON}: row {r.get('name')!r} lacks gate / protocol")
    except (OSError, ValueError) as ex:
        v.append(f"{DEVTOOLS_JSON} unreadable: {ex}")
    for path, lines, _f in parsed:
        rel = os.path.relpath(path, root).replace("\\", "/")
        raw = "\n".join(lines)
        clean = _aht.strip_code(raw)
        # choke point: SR_DEVTOOLS* and dev.txt are read only in kernel/devtools.cpp
        if rel != DEVTOOL_FILE:
            for m in _DEVTOOL_ENV_RE.finditer(raw):
                if clean[m.start():m.start() + 6] == "getenv":
                    v.append(f"{rel}:{raw.count(chr(10), 0, m.start()) + 1} reads {m.group(1)} outside {DEVTOOL_FILE}")
            for m in re.finditer(r'"dev\.txt"', raw):
                if clean[m.start()] == '"':
                    v.append(f"{rel}:{raw.count(chr(10), 0, m.start()) + 1} names dev.txt outside {DEVTOOL_FILE}")
        for m in _DEVTOOL_PRED_RE.finditer(clean):
            ln = raw.count("\n", 0, m.start()) + 1
            if re.search(r"\bbool\s+$", clean[max(0, m.start() - 24):m.start()]):
                continue                                        # the declaration / definition
            o = m.end() - 1
            cl = _paren_close(clean, o)
            if m.group(1) == "devToolAllowed":
                arg = raw[o + 1:cl] if cl > 0 else ""
                lit = re.fullmatch(r'\s*"(\w+)"\s*', arg)
                if not lit:
                    v.append(f"{rel}:{ln} devToolAllowed() with a non-literal argument ({arg.strip()[:40]})")
                elif lit.group(1) not in names:
                    v.append(f"{rel}:{ln} devToolAllowed(\"{lit.group(1)}\") names no tool of the closed set")
            j = m.start()
            while True:                                         # drop sr::kernel:: qualifiers
                q = re.search(r"(\w+\s*::\s*)$", clean[:j])
                if not q:
                    break
                j = q.start()
            before = clean[:j].rstrip()
            after = clean[cl + 1:].lstrip() if cl > 0 else ""
            if before.endswith("||") or after.startswith("||"):
                v.append(f"{rel}:{ln} {m.group(1)}() ORed with another condition (a DEVTOOL gate stands alone)")
        if rel == DEVTOOL_FILE:
            if re.search(r"\bforcingAllowed(?:For)?\s*\(", clean):
                v.append(f"{rel}: the dev-tool gate consults forcingAllowed() (they must stay separate)")
            if re.search(r"!\s*\w+\s*\|\|", clean):
                v.append(f"{rel}: a default-ON idiom (`!e || ...`) in the dev-tool gate")
            v.extend(_devtool_default_off(rel, raw, clean, names))
        else:
            for m in _DEVTOOL_SEAM_RE.finditer(clean):
                v.append(f"{rel}:{raw.count(chr(10), 0, m.start()) + 1} calls the test seam {m.group(1)}() "
                         "(only tools/difftest may set the dev-tool mask)")
        for fm in re.finditer(r"\bbool\s+(forcingAllowed(?:For)?)\s*\([^)]*\)\s*(?:noexcept\s*)?\{", clean):
            cb = _brace_close(clean, fm.end() - 1)
            if cb and re.search(r"\bdevTool\w*\s*\(", clean[fm.end():cb]):
                v.append(f"{rel}: {fm.group(1)}() consults the dev-tool gate (they must stay separate)")
    # generated guest code never names the gate
    gdir = os.path.join(root, GENERATED_DIR)
    if os.path.isdir(gdir):
        for fn in sorted(os.listdir(gdir)):
            if not fn.endswith((".cpp", ".h", ".inc")):
                continue
            with open(os.path.join(gdir, fn), "rb") as f:
                data = f.read()
            for tok in (b"devToolAllowed", b"devToolHookNeeded", b"SR_DEVTOOLS"):
                if tok in data:
                    v.append(f"guest/ppc/{fn} names {tok.decode()} (the gate never enters generated code)")
    # every DEVTOOL site: a closed-set tool, devtool class (an observe tool never writes or calls)
    for s in sites:
        if s[0] != "DEVTOOL":
            continue
        t = re.search(r"SR_DEVTOOLS:(\w+|\?)", s[5] or "")
        tool = t.group(1) if t else "?"
        where = f"{s[1]}:{s[2]} {s[4]}() <{s[3]}>"
        if tool not in names:
            v.append(f"{where}: DEVTOOL site gated by {tool!r}, which is no tool of {DEVTOOLS_JSON}")
        elif cls_of.get(tool) != "devtool":
            v.append(f"{where}: an {cls_of.get(tool)}-class tool ({tool}) writes guest memory / calls guest code")
    if not quiet:
        by = {}
        for s in sites:
            if s[0] == "DEVTOOL":
                t = re.search(r"SR_DEVTOOLS:(\w+|\?)", s[5] or "")
                by[t.group(1) if t else "?"] = by.get(t.group(1) if t else "?", 0) + 1
        print(f"\n--- DEVTOOL CLASS ({DEVTOOL_FILE} closed set: {', '.join(n + '/' + c for n, c in cset)}) ---",
              file=out)
        print(f"  sites by tool: {by or '{}'} (declared, default OFF; not counted as forcing)", file=out)
        for x in v:
            print(f"  VIOLATION {x}", file=out)
    return v


R1_PORT_SUBSTR = "kernel/r1_hooks_"

# R1 (docs/SPLICE_CONVERSION.md): HOOKS ON GENERATED CODE. Until R1, 310 guest functions were
# hand-edited copies of the recompiler output ("splices"), and this audit never saw the
# forcing inside them (7 behaviours live on the default exe). R1 rebuilt the guest code from
# the pristine recompiler output and moved every such behaviour into host code: a
# [[midasm_hook]] in tools/regen/superman_regen.toml (C++ side in kernel/), a strong-def
# override in kernel/r1_*.cpp, or an sr::r1:: helper called from an existing override. The
# write-primitive scan above cannot judge a hook (it bends control flow and registers through
# references), so hooks are judged here, one registry entry each (docs/OVERRIDES.json, keys
# "midasm:0x<ADDR>" / "sub_X"), by the manual `default_exe` field
# (scripts/gen_override_manifest.py DEFAULT_EXE):
#   live / conditional  forcing that ships in the default exe, ungated -> COUNTED in the
#                       forcing number (--count, scripts/forcing_baseline_count.txt)
#   support / observe / platform / gated   not counted (state for another hook, a metric, a
#                       platform fix, behind forcingAllowed())
# FAILS: a hook or R1 port with no registry entry, no `default_exe`, an unknown value, or a
# band-aid without a retirement condition; and any splice file (the build pastes none).
HOOK_TOML = os.path.join("tools", "regen", "superman_regen.toml")
HOOK_COUNTED = ("live", "conditional")
HOOK_VALUES = ("live", "conditional", "support", "observe", "platform", "gated")
SPLICE_FILES = [os.path.join("tools", "regen", n) for n in
                ("splice_bodies.json", "splice_list.txt", "interstitial_blocks.json")]


def hook_inventory(root):
    """[(key, name, file)] for every [[midasm_hook]] and every R1 port override."""
    import tomllib
    sys.path.insert(0, os.path.join(root, "scripts"))
    import gen_override_manifest as _gom
    out = []
    try:
        with open(os.path.join(root, HOOK_TOML), "rb") as fh:
            hooks = tomllib.load(fh).get("midasm_hook", [])
    except (OSError, ValueError):
        hooks = []
    for h in hooks:
        out.append((f"midasm:0x{int(h.get('address', 0)):08X}", h.get("name", "?"), HOOK_TOML))
    kdir = os.path.join(root, "kernel")
    for fn in sorted(os.listdir(kdir)) if os.path.isdir(kdir) else []:
        if not fn.endswith(".cpp"):
            continue
        text = open(os.path.join(kdir, fn), encoding="utf-8", errors="replace").read()
        for m in _gom.DEF_RE.finditer(text):
            sym = m.group(1) or m.group(2)
            body = _gom.extract_body(text, m.end())
            if fn.startswith("r1_") or "sr::r1::" in body or "srGuestLongjmpTryThrow" in body:
                out.append((sym, sym, "kernel/" + fn))
    return out


def hooks_report(root, out=sys.stdout, quiet=False):
    """Print the HOOKS section; return (counted, problems, by_value)."""
    try:
        man = json.load(open(os.path.join(root, "docs", "OVERRIDES.json"), encoding="utf-8"))
    except (OSError, ValueError):
        man = {}
    inv = hook_inventory(root)
    by, problems, rows = {}, [], []
    for key, name, where in inv:
        e = man.get(key)
        if e is None:
            problems.append(f"{key} ({name}, {where}) is not in docs/OVERRIDES.json")
            continue
        d = e.get("default_exe", "")
        if d not in HOOK_VALUES:
            problems.append(f"{key} ({name}) has no valid default_exe ({d!r})")
            continue
        if e.get("class") == "band-aid" and not e.get("retire"):
            problems.append(f"{key} ({name}) is a band-aid without a retirement condition")
        by.setdefault(d, []).append((key, name, e))
        rows.append((d, key, name, e))
    for p in SPLICE_FILES:
        if os.path.exists(os.path.join(root, p)):
            problems.append(f"{p} exists: the build pastes no hand-edited guest bodies (R1)")
    counted = sum(len(by.get(v, [])) for v in HOOK_COUNTED)
    if not quiet:
        tally = ", ".join(f"{v} {len(by.get(v, []))}" for v in HOOK_VALUES)
        print(f"\n--- HOOKS ON GENERATED CODE ({len(inv)}: {len(by.get('live', []))} LIVE + "
              f"{len(by.get('conditional', []))} CONDITIONAL band-aids ship in the default exe and "
              f"count as forcing; {tally}) ---", file=out)
        for v in HOOK_COUNTED:
            print(f"  {v.upper()}:", file=out)
            for key, name, e in sorted(by.get(v, []), key=lambda r: r[1]):
                ev = (e.get("evidence") or "").replace("\n", " ")
                print(f"    {key:20s} {name:34s} {ev[:96]}", file=out)
        for p in problems:
            print(f"  PROBLEM {p}", file=out)
    return counted, problems, {k: len(v) for k, v in by.items()}


ORDER = {"UNGATED": 0, "OWNGATE_ON": 1, "OWNGATE": 2, "MASTER": 3, "DEVTOOL": 3, "HOOK": 3, "ALLOW": 4}


# D7: hand-written guest/*.cpp (guest_harness.cpp, ppc_import_glue.cpp, ...) is
# runtime code like kernel/ and is classified the same way. guest/ppc/** is
# GENERATED and stays with scan_generated (hand edits there, and the committed
# splice bodies in tools/regen/splice_bodies.json) - never this parser.
GUEST_HAND_DIR = "guest"


def scan_files(root):
    """Every source file the classifier reads, in a stable order."""
    out = []
    for d in SCAN_DIRS:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        for dirpath, _, files in os.walk(base):
            for fn in sorted(files):
                if fn.endswith((".cpp", ".cc")):
                    out.append(os.path.join(dirpath, fn))
    g = os.path.join(root, GUEST_HAND_DIR)
    if FIX["D7"] and os.path.isdir(g):
        for fn in sorted(os.listdir(g)):             # top level only: guest/ppc is generated
            p = os.path.join(g, fn)
            if fn.endswith((".cpp", ".cc")) and os.path.isfile(p):
                out.append(p)
    if FIX["D16"]:
        # D16: hand-written guest code compiled into the guest library
        # (guest/CMakeLists.txt: ppc/ppc_missing_recomp_stubs.cpp is assembled
        # from this committed source) - its PPC_FUNC bodies are strong defs
        # that REPLACE recompiled guest functions
        for rel in HAND_GUEST_SOURCES:
            p = os.path.join(root, rel)
            if os.path.isfile(p):
                out.append(p)
    return out


# D16: committed hand-written guest sources (tools/regen/assemble.py copies them
# into the gitignored guest/ppc), and the header whose PPC_CALL_INDIRECT_FUNC
# macro every generated indirect call expands.
HAND_GUEST_SOURCES = ["tools/regen/hand/ppc_missing_recomp_stubs.cpp"]
HAND_CONTEXT_H = "tools/regen/hand/ppc_context.h"
# a guest REGISTER written by hand-written guest code: a fabricated return
# value (`ctx.r3.u32 = 0` standing in for a guest function / call)
HAND_REG_RE = re.compile(r"\bctx\s*\.\s*(?:r\d+|f\d+|v\d+|ctr|lr|cr\d*|xer)\b(?:\s*\.\s*\w+)*\s*=(?!=)")


def hand_register_writes(ctx, path):
    """{pos: 'fake_return'} of guest-register writes inside the PPC_FUNC bodies
    of a hand-written guest source (D16)."""
    s = ctx.src(path)
    out = {}
    for fs, fe, sig in ctx.by_path[path][1]:
        if not re.search(r"\bPPC_FUNC(?:_IMPL)?\s*\(|\bPPCContext\s*&", sig):
            continue
        lo = s._line_starts[fs]
        hi = s._line_starts[fe + 1] if fe + 1 < len(s._line_starts) else len(s.clean)
        for m in HAND_REG_RE.finditer(s.clean, lo, hi):
            out[m.start()] = "fake_return"
    return out


_OVERRIDE_NAME_RE = re.compile(r"\b(sub_[0-9A-Fa-f]{8})\b")
_OVERRIDE_SIG_RE = re.compile(r"\bPPC_FUNC(?:_IMPL)?\s*\(|\bPPCContext\s*&")
# what ends a fake-return path: a `return`, or a call of the guest body itself
_GUEST_BODY_OR_RETURN_RE = re.compile(r"\breturn\b|\b__imp__sub_[0-9A-Fa-f]{8}\s*\(")


def is_override_host_file(rel):
    """D20: kernel/** and the hand-written top-level guest/*.cpp (not guest/ppc,
    not tools/regen/hand - D16 covers that)."""
    rel = rel.replace("\\", "/")
    return rel.startswith("kernel/") or (rel.startswith(GUEST_HAND_DIR + "/") and rel.count("/") == 1)


_MACRO_HELPERS_CACHE = {}


def macro_called_helpers(root):
    """D20: host functions the generated indirect-call dispatcher calls with the
    guest context (tools/regen/hand/ppc_context.h, `sr::kernel::X(ctx, ...)` in
    PPC_CALL_INDIRECT_FUNC) - e.g. indirectCallUnresolved, the one place an
    unresolvable bctrl lands. They run as part of EVERY guest indirect call, so a
    register they write is a fabricated guest result exactly like an override's."""
    if root in _MACRO_HELPERS_CACHE:
        return _MACRO_HELPERS_CACHE[root]
    names = set()
    try:
        raw = open(os.path.join(root, HAND_CONTEXT_H), encoding="utf-8", errors="replace").read()
    except OSError:
        raw = ""
    m = re.search(r"#define\s+PPC_CALL_INDIRECT_FUNC\s*\([^)]*\)", raw)
    if m:
        end = m.end()
        while True:
            nl = raw.find("\n", end)
            if nl < 0:
                nl = len(raw)
            if raw[:nl].rstrip().endswith("\\"):
                end = nl + 1
                continue
            end = nl
            break
        for q in re.finditer(r"(?:sr::kernel::)?([A-Za-z_]\w*)\s*\(\s*ctx\s*[,)]", raw[m.end():end]):
            names.add(q.group(1))
    _MACRO_HELPERS_CACHE[root] = names
    return names


def override_fake_returns(ctx, path):
    """D20: {pos: 'fake_return'} of every guest-register write inside a strong-def
    override of a guest function (or a host helper the indirect-call dispatcher
    calls with ctx) whose path reaches a `return` (or the end of the function)
    WITHOUT first calling the guest body `__imp__sub_X(`. A write that only
    feeds the guest body (an argument set up just before `__imp__`) is not one;
    a result substituted after the body ran is."""
    s = ctx.src(path)
    out = {}
    helpers = macro_called_helpers(ctx.root) if hasattr(ctx, "root") else macro_called_helpers(ROOT)
    for fs, fe, sig in ctx.by_path[path][1]:
        if __imp_name(sig) or not _OVERRIDE_SIG_RE.search(sig):
            continue
        hname = re.search(r"\b([A-Za-z_]\w*)\s*\(", sig)
        is_helper = bool(hname and hname.group(1) in helpers)
        if not _OVERRIDE_NAME_RE.search(sig) and not is_helper:
            continue
        lo = s._line_starts[fs]
        hi = s._line_starts[fe + 1] if fe + 1 < len(s._line_starts) else len(s.clean)
        for m in HAND_REG_RE.finditer(s.clean, lo, hi):
            nxt = _GUEST_BODY_OR_RETURN_RE.search(s.clean, m.end(), hi)
            if nxt is None or not nxt.group(0).startswith("__imp__"):
                out[m.start()] = "fake_return"
    return out


def __imp_name(sig):
    """The override's own name is `__imp__sub_X` (a generated body, not an override)."""
    m = re.search(r"\b(?:PPC_FUNC(?:_IMPL)?\s*\(\s*|void\s+)(__imp__\w+)", sig)
    return bool(m)


class TextSrc(_aht.Src):
    """audit_host_threads.Src over TEXT (a macro body, a splice body) instead of
    a file, so SiteGates can judge gates there too (D16 / D18)."""

    def __init__(self, label, text, root):
        self.path = label
        self.rel = os.path.relpath(label, root).replace("\\", "/")
        self.raw = text
        self.clean = _aht.strip_code(text)
        self._line_starts = [0] + [m.end() for m in re.finditer("\n", text)]
        self._match_braces()
        self._index_functions()


def indirect_call_sites(ctx, root):
    """D16: the branches of PPC_CALL_INDIRECT_FUNC (tools/regen/hand/ppc_context.h)
    that do NOT dispatch the guest's target: a fabricated return value, or a
    host stub standing in for guest code. Each is a site in the header, judged
    by SiteGates over the macro body (continuations joined; line numbers kept)."""
    p = os.path.join(root, HAND_CONTEXT_H)
    try:
        raw = open(p, encoding="utf-8", errors="replace").read()
    except OSError:
        return []
    m = re.search(r"#define\s+PPC_CALL_INDIRECT_FUNC\s*\([^)]*\)", raw)
    if not m:
        return []
    end = m.end()
    while True:                                    # the macro ends at a line without `\`
        nl = raw.find("\n", end)
        if nl < 0:
            nl = len(raw)
        if raw[:nl].rstrip().endswith("\\"):
            end = nl + 1
            continue
        end = nl
        break
    # blank everything outside the macro body, keep offsets (so lines map 1:1)
    body = "".join(ch if (m.end() <= i < end or ch == "\n") else " " for i, ch in enumerate(raw))
    body = body.replace("\\\n", " \n")
    src = TextSrc(p + "#PPC_CALL_INDIRECT_FUNC", body, root)
    ctx._src[src.path] = src
    out = []
    lines = raw.split("\n")
    hits = [(q.start(), "fake_return") for q in HAND_REG_RE.finditer(src.clean)]
    hits += [(q.start(), "guest_call") for q in re.finditer(
        r"(?<![\w.>])(?!_fn\b)([A-Za-z_]\w*)\s*\(\s*ctx\s*,\s*_?base\s*\)", src.clean)]
    for pos, kind in sorted(hits):
        idx = src.line_of(pos) - 1
        lvl, env, conds = ctx.gates_site.level_at(src, pos)
        cond = "; ".join(c for c in conds) or "(no condition)"
        if lvl == DEAD:
            cls, det = "OWNGATE", "observe-only(dead)"
        elif lvl == MASTER:
            cls, det = "MASTER", ""
        elif lvl == DEVTOOL:
            cls, det = "DEVTOOL", env
        elif lvl == OFF:
            cls, det = "OWNGATE", env
        else:
            cls, det = ("OWNGATE_ON" if lvl == ON else "UNGATED"), (
                "indirect-call branch that does not dispatch the guest target: " + cond)[:160]
            for fsub, anchor, reason in ALLOWLIST:
                if fsub in HAND_CONTEXT_H and allow_anchor_hit(anchor, "PPC_CALL_INDIRECT_FUNC", lines[idx], ""):
                    cls, det = "ALLOW", reason
                    break
        out.append((cls, HAND_CONTEXT_H, idx + 1, kind, "PPC_CALL_INDIRECT_FUNC", det,
                    lines[idx].strip()[:100], pos - src._line_starts[idx]))
    return out


def parse_tree(root):
    """Pass 1: read every scanned file and parse its functions."""
    parsed = []
    for path in scan_files(root):
        try:
            lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
        except OSError:
            continue
        parsed.append((path, lines, parse_functions(lines)))
    return parsed


_WG_DEF_RE = re.compile(r"\b(?:inline\s+)?(?:void|uint32_t|bool)\s+$")


def site_positions(ctx, path, rel, gw):
    """D12: {pos: kind} of EVERY write occurrence in the file (clean-text offsets)."""
    s = ctx.src(path)
    c = s.clean
    out = {}
    prims = list(PRIMS) + ([] if gw is not None else (D7_PRIMS if FIX["D7"] else []))
    if rel.startswith(HOST_SYNC_DIRS):
        prims.append(HOST_SYNC_PRIM)
    for kind, rx in prims:
        for m in rx.finditer(c):
            ls = s._line_starts[s.line_of(m.start()) - 1]
            if kind == "guest_write" and _WG_DEF_RE.search(c[ls:m.start()]):
                continue                     # a writeGuest* helper DEFINITION
            out.setdefault(m.start(), kind)
    if gw is not None:
        for pos, kind in gw.sites(path).items():
            out.setdefault(pos, kind)
        if HAND_REG_RE is not None and FIX["D16"] and rel.startswith(HAND_GUEST_PREFIXES[1]):
            for pos, kind in hand_register_writes(ctx, path).items():
                out.setdefault(pos, kind)
        if FIX["D20"] and is_override_host_file(rel):
            for pos, kind in override_fake_returns(ctx, path).items():
                out.setdefault(pos, kind)
    elif FIX["D7"]:
        lines_ = s.raw.split("\n")
        funcs = ctx.by_path[path][1]
        for fs, fe, sig in funcs:
            lo = s._line_starts[fs]
            hi = s._line_starts[fe + 1] if fe + 1 < len(s._line_starts) else len(c)
            base_ok = rel.startswith(GUEST_HAND_DIR + "/") or bool(
                re.search(r"uint8_t\s*\*\s*(?:__restrict\s+)?base\b", sig))
            for k, v in derived_store_lines(s, lo, hi, base_ok).items():
                m = re.search(r"\S", lines_[k])
                p = s._line_starts[k] + (m.start() if m else 0)
                if not any(s._line_starts[k] <= q < s._line_starts[k] + len(lines_[k]) for q in out):
                    out.setdefault(p, v)
        if rel.startswith(GUEST_HAND_DIR + "/"):
            for m in re.finditer(r"\b(?:std::)?mem(set|cpy|move)\s*\(\s*(?:base|gb)\b", c):
                out.setdefault(m.start(), "guest_memset" if m.group(1) == "set" else "guest_memcpy")
    return out


def collect(root=ROOT):
    """Parse the tree under root and classify every primitive site.
    Returns (parsed, sites); a site is (cls, rel, line, prim, func, detail, code)."""
    parsed = parse_tree(root)
    ctx = AuditCtx(parsed, root)
    LAST_CTX["ctx"] = ctx

    # Pass 2: find primitive sites (matched on COMMENT/STRING-STRIPPED code) + classify.
    sites = []
    gw = GuestWrites(ctx) if (FIX["D7"] and FIX["D11"]) else None
    for path, lines, funcs in parsed:
        rel = ctx.rel(path)
        if FIX["D12"]:
            # D12: EVERY write occurrence is its own site, judged at its own
            # position (`} else h[0] = 1;`, `if (M) w(a); else w(b);`)
            for pos, kind in sorted(site_positions(ctx, path, rel, gw).items()):
                s = ctx.src(path)
                idx = s.line_of(pos) - 1
                cls, detail, fname = classify(ctx, path, lines, funcs, idx, kind, pos=pos)
                sites.append((cls, rel, idx + 1, kind, fname, detail, lines[idx].strip()[:100],
                              pos - s._line_starts[idx]))
            continue
        in_block = False
        derived = {}
        if gw is not None:
            s = ctx.src(path)
            for pos, kind in gw.sites(path).items():
                derived.setdefault(s.line_of(pos) - 1, kind)
        elif FIX["D7"]:
            s = ctx.src(path)
            for fs, fe, sig in funcs:
                lo = s._line_starts[fs]
                hi = s._line_starts[fe + 1] if fe + 1 < len(s._line_starts) else len(s.clean)
                base_ok = rel.startswith(GUEST_HAND_DIR + "/") or bool(
                    re.search(r"uint8_t\s*\*\s*(?:__restrict\s+)?base\b", sig))
                for k, v in derived_store_lines(s, lo, hi, base_ok).items():
                    derived.setdefault(k, v)
        for idx, line in enumerate(lines):
            clean, in_block = strip_for_braces(line, in_block)
            # skip helper DEFINITIONS of write wrappers (a def, not a forcing call)
            if re.search(r"\b(inline\s+)?(void|uint32_t|bool)\s+writeGuest\w+\s*\(", clean):
                continue
            prims = all_prims()
            if rel.startswith(HOST_SYNC_DIRS):
                prims.append(HOST_SYNC_PRIM)
            hit = next((kind for kind, rx in prims if rx.search(clean)), None)
            if hit is None and FIX["D7"] and rel.startswith(GUEST_HAND_DIR + "/") and re.search(
                    r"\b(?:std::)?mem(?:set|cpy|move)\s*\(\s*(?:base|gb)\b", clean):
                hit = "guest_memset" if "memset" in clean else "guest_memcpy"
            hit = hit or derived.get(idx)
            if hit:
                cls, detail, fname = classify(ctx, path, lines, funcs, idx, hit)
                sites.append((cls, rel, idx + 1, hit, fname, detail, line.strip()[:100], 0))
    order = ORDER
    # iter-463: CALLER PROPAGATION. Gate detection is per enclosing function, so
    # a write in a helper (makeLatchObj, wr32, ...) whose every caller is gated
    # read UNGATED - the old parser hid this only because whole namespaces were
    # one "function". A helper inherits the WEAKEST class among the functions
    # that reference it (a thread main passed to std::thread counts as a
    # reference). No reference at all (outside its own body / a prototype) =
    # dead code, reported as such.
    memo = {}
    porder = ({"UNGATED": 0, "OWNGATE_ON": 1, "ALLOW": 2, "OWNGATE": 3, "MASTER": 4, "DEVTOOL": 4, "HOOK": 4}
              if FIX["D5"] else ORDER)

    def func_class(fname, kind, depth=0):
        key = (fname, kind)
        if key in memo:
            return memo[key]
        if depth > 5 or fname in ("(file scope)", "?") or not re.match(r"^\w+$", fname):
            return ("UNGATED", "")
        memo[key] = ("UNGATED", "")            # cycle guard
        call_re = re.compile(r"\b" + re.escape(fname) + r"\s*\(")
        proto_re = _proto_re(fname)
        worst, detail, nrefs = ("MASTER" if FIX["D5"] else "ALLOW"), "", 0
        for path, idx, clean in ctx.refs(fname):
            if proto_re.match(clean):
                continue
            lines, funcs = ctx.by_path[path]
            occs = [None]
            if FIX["D12"]:
                # D12: every reference on the line is its own caller site
                # (`if (M) helper(a); else helper(b);`)
                s_ = ctx.src(path)
                ls_ = s_._line_starts[idx]
                le_ = s_._line_starts[idx + 1] if idx + 1 < len(s_._line_starts) else len(s_.clean)
                occs = [ls_ + m.start() for m in re.finditer(r"\b" + re.escape(fname) + r"\b",
                                                              s_.clean[ls_:le_])
                        # obj.load() / p->load() is a MEMBER, not this free function
                        if not re.search(r"(?:\.|->)\s*$", s_.clean[max(0, ls_ + m.start() - 4):ls_ + m.start()])]
                if not occs:
                    continue
            for pos in occs:
                if pos is None:
                    is_call = bool(call_re.search(clean))
                else:
                    is_call = bool(re.match(r"\s*\(", s_.clean[pos + len(fname):pos + len(fname) + 40]))
                cls, det, caller = classify(ctx, path, lines, funcs, idx, kind, at=fname,
                                            entry=not is_call, pos=pos)
                if caller == fname:
                    continue                    # its own body (recursion)
                if caller == "(file scope)":
                    if not FIX["D9"] or _def_head(funcs, idx, fname):
                        continue                # a definition / prototype line
                    # D9: a callback table / static initializer at file scope runs
                    # the function with no gate of its own
                    cls, det = "UNGATED", f"file-scope reference {ctx.rel(path)}:{idx + 1}"
                nrefs += 1
                if cls == "UNGATED" and caller != "(file scope)" and not det.startswith(EXPORT_THREAD):
                    cls, det = func_class(caller, kind, depth + 1)
                # A NON-call reference (a thread entry: std::thread(F), a
                # callback table) starts a context of its own: an export that
                # spawns a background thread does not make the thread's
                # fabrication part of the export's contract. Such a reference
                # passes on a gate, never an ALLOW.
                if not is_call and cls == "ALLOW":
                    cls, det = "UNGATED", f"thread/callback entry, not a call"
                if porder[cls] < porder[worst]:
                    worst, detail = cls, (det or "") + f" via {caller}()"
        if FIX["D17"]:
            # D17: references from generated guest code / headers / hand-written
            # guest code the classifier does not parse
            for label, pos in ctx.external_refs(fname):
                r = ctx.external_ref_class(fname, label, pos)
                if r is None:
                    continue
                nrefs += 1
                if porder[r[0]] < porder[worst]:
                    worst, detail = r[0], r[1]
        if FIX["D21"] and nrefs > 0 and _guest_called(ctx, fname) and porder["UNGATED"] < porder[worst]:
            # D21: generated guest code calls an override of a guest function
            # with no gate at all; a gated HOST reference (a probe that takes
            # its address under forcingAllowed()) does not make it master-only
            worst, detail = "UNGATED", "guest-called override (generated code calls it ungated)"
        if nrefs == 0 and FIX["D9"] and (re.match(r"^(?:__imp__\w+|sub_[0-9A-Fa-f]{8})$", fname)
                                         or fname in ctx.guest_callable):
            # D9: an override of a guest function / an import thunk is called by
            # GENERATED code, which the reference index does not read
            res = ("UNGATED", "guest-called override (no host caller)")
        elif nrefs == 0 and FIX["D9"] and fname in ("main", "SDL_main", "WinMain", "wWinMain", "wmain"):
            res = ("UNGATED", "program entry")
        elif nrefs == 0:
            res = ("OWNGATE", "dead code: no reference")
        elif worst == "ALLOW":
            res = ("ALLOW", "every caller that runs by default is an export / allowlisted")
        else:
            res = (worst, detail.strip())
        memo[key] = res
        return res

    for i, st in enumerate(sites):
        if st[0] == "UNGATED" and st[4] not in ("(file scope)",) and not st[5].startswith(EXPORT_THREAD):
            cls, det = func_class(st[4], st[3])
            if cls != "UNGATED":
                sites[i] = (cls, st[1], st[2], st[3], st[4], "caller-gated: " + det, st[6], st[7])
            elif det:
                sites[i] = (cls, st[1], st[2], st[3], st[4], det, st[6], st[7])
    # B4: an own-gated function is still MASTER when it can only be reached
    # through the master gate: every reference (a call, or a thread entry such as
    # std::thread(feSmKickThread) inside if (forcingAllowed())) sits in a
    # master-gated branch / after a master early-return (audit_host_threads.gate_of,
    # site-local, not "forcingAllowed somewhere in the caller"), or in a function
    # that is itself only master-reached.
    # (An UNGATED site whose propagation stopped at a thread entry launched from
    # an export-only helper - advanceFencesIfStalled via startGpuInterruptThread,
    # which returns early unless forcingAllowed() - is lifted the same way.)
    if FIX["B4"]:
        for i, st in enumerate(sites):
            if st[0] in ("UNGATED", "OWNGATE_ON", "OWNGATE") and st[4] not in ("(file scope)",):
                if FIX["D21"] and _guest_called(ctx, st[4]):
                    continue                    # D21: generated code reaches it ungated
                ok, via = ctx.master_reached(st[4])
                if ok:
                    sites[i] = ("MASTER", st[1], st[2], st[3], st[4],
                                f"caller-gated: only reached under the master gate ({via})", st[6], st[7])
    if FIX["D16"]:
        # the generated indirect-call dispatcher: expanded at every generated
        # `bctrl`, so it runs from guest code (no host caller to propagate from)
        sites += indirect_call_sites(ctx, root)
    sites.sort(key=lambda s: (order[s[0]], s[1], s[2], s[7] if len(s) > 7 else 0))
    return parsed, sites


def _guest_called(ctx, fname):
    """D21: an override of a guest function / an import thunk: generated guest
    code calls it (by name or through the dispatch table) with no gate."""
    return bool(re.match(r"^(?:__imp__\w+|sub_[0-9A-Fa-f]{8})$", fname)) or fname in ctx.guest_callable


def generated_call_writers(sites):
    """Functions whose call from generated guest code is a hand edit that writes
    guest state (D18). D20 (iter-463 burndown): a function whose EVERY write site
    is gated INSIDE the function itself - MASTER / ALLOW by its own gate, not
    "caller-gated" (which only reflects host callers) - writes nothing unless its
    own gate opens, so the generated call site is not an ungated writer. The
    indirect-call dispatcher's indirectCallUnresolved (legacy r3=0 only under
    SR_ALLOW_FORCING) is the case this exists for."""
    if not FIX["D20"]:
        return {s[4] for s in sites}
    selfgated = {}
    for s in sites:
        ok = s[0] in ("MASTER", "ALLOW", "DEVTOOL", "HOOK") and not s[5].startswith("caller-gated")
        selfgated[s[4]] = selfgated.get(s[4], True) and ok
    return {f for f, g in selfgated.items() if not g}


def load_band_aid_syms(root):
    """Overrides docs/OVERRIDES.json classifies band-aid.

    iter-463 round 2: this lived inline in main(), whose `--json` branch did
    `import json` - which makes `json` LOCAL to all of main(), so this load
    raised UnboundLocalError, the bare `except` swallowed it, and the
    "band-aid shipping by default" failure could never fire."""
    with open(os.path.join(root, "docs", "OVERRIDES.json"), encoding="utf-8") as fh:
        return {sym for sym, ent in json.load(fh).items()
                if isinstance(ent, dict) and ent.get("class") == "band-aid"}


# iter-463 burndown-2 REVIEW: control-flow divergences of the splice bodies from
# the PRISTINE recompiler output. scan_generated (above) sees only hand-written
# C++ that NAMES something (sr::kernel:: calls, injected stores / guest calls);
# its docstring states the limit: a hand edit that DELETES generated code or
# adds a forced early return "leaves nothing to match; only a diff against the
# pristine tools/regen/ppc_out shows it". So the splice section printed
# "0 ungated" while the committed bodies carried default-ON skips of whole
# guest functions (SR_ANIM_STUB, sub_823C8CC0's `#if 0` body, sub_82703EA0's
# null-call bail, ...). This pass diffs every splice body against its pristine
# function and reports, per function:
#   EXIT - an ADDED line with `return;` / `goto loc_` (a forced early exit or a
#          new branch; `ctx.r3 = ...` right above it makes it a fabricated result);
#   DROP - pristine control-flow lines (branch, return, guest call) absent from
#          the splice body (a `#if 0` block is dropped code too), i.e. guest
#          behavior deleted. Replaced COMPUTATION lines (the hand VMX bodies,
#          reservation fixes) are data-path codegen fixes and are not counted.
# Each is judged by SiteGates inside its function (generated_level); level >= OFF
# (default-OFF own gate, master gate, dead) is gated. Everything else is an
# UNREVIEWED divergence: not necessarily forcing (recompiler-bug fixes live here
# too), but nothing proves it is not. The count is ratcheted against
# scripts/splice_divergence_baseline.txt so it can only go DOWN.
SPLICE_PRISTINE_DIR = os.path.join("tools", "regen", "ppc_out")
SPLICE_DIV_BASELINE = os.path.join("scripts", "splice_divergence_baseline.txt")
_SPLICE_EXIT_RE = re.compile(r"\breturn\s*;|\bgoto\s+loc_[0-9A-Fa-f]+\s*;")


def _pristine_functions(root):
    d = os.environ.get("SR_AUDIT_PRISTINE_DIR") or os.path.join(root, SPLICE_PRISTINE_DIR)
    if not os.path.isdir(d):
        return None, d
    out = {}
    for fn in sorted(os.listdir(d)):
        if not re.match(r"ppc_recomp\.\d+\.cpp$", fn):
            continue
        cur, buf = None, []
        with open(os.path.join(d, fn), "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.match(r"PPC_FUNC_IMPL\(__imp__(\w+)\)", line)
                if m:
                    cur, buf = m.group(1), [line]
                    continue
                if cur is not None:
                    buf.append(line)
                    if line.startswith("}"):
                        out[cur] = buf
                        cur = None
    return out, d


def _drop_if0(lines):
    """(kept_lines, kept_index_map): the lines outside `#if 0 ... #endif`."""
    kept, idx, depth = [], [], 0
    for i, l in enumerate(lines):
        s = l.strip()
        if depth:
            if re.match(r"#\s*if", s):
                depth += 1
            elif re.match(r"#\s*endif", s):
                depth -= 1
            continue
        if re.match(r"#\s*if\s+0\b", s):
            depth = 1
            continue
        kept.append(l)
        idx.append(i)
    return kept, idx


_FLOW_LINE_RE = re.compile(r"\bgoto\b|\breturn\b|\bsub_[0-9A-Fa-f]{8}\s*\(|__imp__\w+\s*\(|"
                           r"PPC_CALL_INDIRECT_FUNC|__restgprlr_\d+\s*\(|__savegprlr_\d+\s*\(")


def _is_code(s):
    """A pristine CONTROL-FLOW line (branch, return, guest call). Dropped
    computation lines (the hand VMX re-implementations, lwarx reservation
    fixes) are codegen fixes of the data path, not skipped guest behavior."""
    s = s.strip()
    return bool(s) and not s.startswith("//") and bool(_FLOW_LINE_RE.search(s))


def splice_divergence(root, ctx):
    """None when the pristine tree is absent, else (rows, n_functions_compared)
    with rows = [(name, line, kind, gated, text)] (line: 1-based in the body)."""
    import difflib
    pristine, _d = _pristine_functions(root)
    if pristine is None:
        return None
    try:
        bodies = json.load(open(os.path.join(root, SPLICE_BODIES), encoding="utf-8"))
    except (OSError, ValueError):
        return [], 0
    rows, compared = [], 0
    for name in sorted(bodies):
        body = bodies[name]
        if not isinstance(body, str) or name not in pristine:
            continue
        compared += 1
        lines = body.splitlines(True)
        kept, kidx = _drop_if0(lines)
        a = [l.strip() for l in pristine[name]]
        b = [l.strip() for l in kept]
        offs = [0]
        for l in lines:
            offs.append(offs[-1] + len(l))
        label = f"{SPLICE_BODIES}#{name}"

        def gated(i, col=None):
            if ctx is None:
                return False
            return generated_level(ctx, label, lines, offs, min(i, len(lines) - 1), col) >= OFF
        # a whole `#if 0` block is one DROP at its `#if 0` line
        kset = set(kidx)
        if0_lines = {l.strip() for k, l in enumerate(lines) if k not in kset}
        depth = 0
        for i, l in enumerate(lines):
            s = l.strip()
            if not depth and re.match(r"#\s*if\s+0\b", s):
                rows.append((name, i + 1, "DROP", gated(max(i - 1, 0)), "#if 0 (generated body disabled)"))
                depth = 1
            elif depth:
                if re.match(r"#\s*if", s):
                    depth += 1
                elif re.match(r"#\s*endif", s):
                    depth -= 1
        sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
        for op, i1, i2, j1, j2 in sm.get_opcodes():
            if op in ("insert", "replace"):
                for j in range(j1, j2):
                    mx = _SPLICE_EXIT_RE.search(_aht.strip_code(kept[j]))
                    if mx:
                        li = kidx[j]
                        rows.append((name, li + 1, "EXIT", gated(li, mx.start()), b[j][:110]))
            if op in ("delete", "replace"):
                dropped = [s for s in a[i1:i2] if _is_code(s)]
                # a replaced line that survives elsewhere in the body (moved) is not dropped
                dropped = [s for s in dropped if s not in b and s not in if0_lines]
                if dropped:
                    li = kidx[j1] if j1 < len(kidx) else (kidx[-1] if kidx else 0)
                    rows.append((name, li + 1, "DROP", gated(li),
                                 f"{len(dropped)} generated line(s), first: {dropped[0][:90]}"))
    return rows, compared


def splice_divergence_report(root, ctx, out=sys.stdout):
    """Print the pass; return (ungated_count or None when skipped, baseline)."""
    res = splice_divergence(root, ctx)
    try:
        base = int(open(os.path.join(root, SPLICE_DIV_BASELINE)).read().split()[0])
    except (OSError, ValueError, IndexError):
        base = None
    if res is None:
        print(f"\n--- SPLICE BODIES vs PRISTINE: SKIPPED (no {SPLICE_PRISTINE_DIR}; set "
              f"SR_AUDIT_PRISTINE_DIR or regenerate per tools/regen/README.md) ---", file=out)
        return None, base
    rows, compared = res
    ung = [r for r in rows if not r[3]]
    fns = sorted({r[0] for r in ung})
    print(f"\n--- SPLICE BODIES vs PRISTINE ({compared} spliced functions compared with "
          f"{SPLICE_PRISTINE_DIR}): {len(rows)} control-flow divergence(s), "
          f"{len(ung)} UNREVIEWED (not behind a default-OFF / master gate) in {len(fns)} "
          f"function(s); baseline {base} ---", file=out)
    for name in fns:
        mine = [r for r in ung if r[0] == name]
        ex = sum(1 for r in mine if r[2] == "EXIT")
        dr = sum(1 for r in mine if r[2] == "DROP")
        first = mine[0]
        print(f"  {name}: {ex} exit(s), {dr} drop(s)  first @{first[1]} {first[2]}: {first[4]}", file=out)
    return len(ung), base


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "--summary"
    parsed, sites = collect(ROOT)
    order = ORDER
    counts = {k: 0 for k in order}
    for s in sites:
        counts[s[0]] += 1

    flagged = [s for s in sites if s[0] == "UNGATED"]
    defon = [s for s in sites if s[0] == "OWNGATE_ON"]
    owngate = [s for s in sites if s[0] == "OWNGATE"]

    if mode == "--count":            # machine-readable UNGATED count (for the hook)
        iso = isolation_report(parsed, quiet=True)
        for v in iso:
            print(f"ISOLATION VIOLATION: {v}", file=sys.stderr)
        dtv = devtool_report(parsed, ROOT, sites, quiet=True)
        for v in dtv:
            print(f"DEVTOOL VIOLATION: {v}", file=sys.stderr)
        iso = iso + dtv
        # iter-463 review: the splice-divergence ratchet rides along (stderr).
        sd_n, sd_base = (splice_divergence_report(ROOT, LAST_CTX.get("ctx"), out=sys.stderr)
                         if os.path.exists(os.path.join(ROOT, SPLICE_BODIES)) else (None, None))
        sd_bad = sd_n is not None and sd_base is not None and sd_n > sd_base
        if sd_bad:
            print(f"SPLICE DIVERGENCE RATCHET: {sd_n} > baseline {sd_base}", file=sys.stderr)
        # R1: the forcing number = UNGATED write sites + the live / conditional band-aid hooks
        # on generated code (hooks_report). scripts/forcing_baseline_count.txt ratchets it.
        hk_n, hk_problems, hk_by = hooks_report(ROOT, quiet=True)
        for p in hk_problems:
            print(f"HOOK REGISTRY: {p}", file=sys.stderr)
        print(f"forcing = {len(flagged)} UNGATED + {hk_n} hook band-aids "
              f"(live {hk_by.get('live', 0)}, conditional {hk_by.get('conditional', 0)})", file=sys.stderr)
        print(len(flagged) + hk_n)
        return 1 if (flagged or iso or sd_bad or hk_problems) else 0

    if mode == "--splice-ratchet":   # iter-463 review: the pre-commit hook's second check
        # R1: there are no splice bodies; a splice file coming back fails (hooks_report).
        _n, hk_problems, _by = hooks_report(ROOT)
        sd_n, sd_base = splice_divergence_report(ROOT, LAST_CTX.get("ctx"))
        if sd_n is not None and sd_base is not None and sd_n > sd_base:
            print(f"SPLICE DIVERGENCE RATCHET: {sd_n} > baseline {sd_base} ({SPLICE_DIV_BASELINE})")
            return 1
        return 1 if any("pastes no hand-edited" in p for p in hk_problems) else 0

    if mode == "--isolation":        # standalone choke-point report
        return 1 if isolation_report(parsed) else 0

    if mode == "--json":
        print(json.dumps({"counts": counts,
                          "sites": [dict(zip(["cls","file","line","prim","func","detail","code","col"], s)) for s in sites]}, indent=2))
        return 1 if flagged else 0

    print("=" * 78)
    print("FORCING AUDIT  (scripts/audit_forcing.py) -- guest-state-writing / guest-driving sites")
    print("=" * 78)
    print(f"  total sites: {len(sites)}")
    print(f"    UNGATED  (FLAGGED for review -- possible hidden band-aid): {counts['UNGATED']}")
    print(f"    DEFAULT-ON (private SR_* env that defaults ON -- FORCING THAT SHIPS): {counts['OWNGATE_ON']}")
    print(f"    OWNGATE  (private SR_* env, default OFF -- opt-in diagnostic): {counts['OWNGATE']}")
    print(f"    MASTER   (behind forcingAllowed() -- OK): {counts['MASTER']}")
    print(f"    DEVTOOL  (behind devToolAllowed(<tool>) -- declared dev tool, default OFF): {counts['DEVTOOL']}")
    print(f"    HOOK     (write inside a hook on generated code; judged per hook below): {counts['HOOK']}")
    print(f"    ALLOW    (reviewed platform-fix allowlist -- OK): {counts['ALLOW']}")
    print()

    def dump(title, rows):
        print(f"--- {title} ({len(rows)}) ---")
        for cls, rel, ln, kind, fname, detail, code, *_col in rows:
            tag = f" [{detail}]" if detail else ""
            print(f"  {rel}:{ln}  {fname}()  <{kind}>{tag}")
            print(f"      {code}")
        print()

    if mode in ("--all",):
        dump("MASTER-gated", [s for s in sites if s[0] == "MASTER"])
        dump("DEVTOOL-gated (declared dev tools)", [s for s in sites if s[0] == "DEVTOOL"])
        dump("ALLOWLISTED platform-fix", [s for s in sites if s[0] == "ALLOW"])
    dump("DEFAULT-ON own-gate (FORCING THAT SHIPS -- fold onto the master gate)", defon)
    dump("OWN-GATE, default OFF (opt-in diagnostic; fold onto the master gate)", owngate)
    dump("UNGATED -- FLAGGED (gate behind forcingAllowed() or allowlist w/ reason)", flagged)

    # iter-463 round 2: every check below ALWAYS runs; the exit code combines
    # them. (It used to return at the first failure, so with an UNGATED site
    # the generated-code, splice, registry and isolation checks went silent.)
    failed = []
    hk_n, hk_problems, hk_by = hooks_report(ROOT)
    print(f"\nFORCING NUMBER: {len(flagged)} UNGATED + {hk_n} hook band-aids (live "
          f"{hk_by.get('live', 0)}, conditional {hk_by.get('conditional', 0)}) = "
          f"{len(flagged) + hk_n} (ratchet: scripts/forcing_baseline_count.txt)")
    if hk_problems:
        print(f"RESULT: {len(hk_problems)} hook registry problem(s) (above).")
        failed.append(f"{len(hk_problems)} hook registry")
    if flagged:
        print(f"RESULT: {len(flagged)} UNGATED site(s) need review. NOT a clean forcing-off default.")
        failed.append(f"{len(flagged)} UNGATED")

    # iter-402: a private SR_* gate that DEFAULTS ON is forcing that ships. The
    # charter permits forcing only as a named, opt-in diagnostic, so default-ON
    # own-gates fail the audit exactly like ungated ones. Before this, the exit
    # code keyed on UNGATED alone and a run with 83 own-gated sites printed
    # "Default exe forces nothing" -- true only of the category it counted.
    # Not every default-ON site is forcing: a PLATFORM FIX is supposed to be on
    # by default (it implements what the console provided). The line between the
    # two already exists and is reviewed -- it lives in docs/OVERRIDES.json. So
    # fail only where a default-ON site sits inside an override classified
    # band-aid: behavior that is NOT console parity, shipping by default. The
    # rest are reported loudly but do not fail, because failing them would
    # demand that genuine platform fixes be switched off by default.
    band_aid_syms = load_band_aid_syms(ROOT)
    shipping_band_aids = []
    for site in defon:
        fname = site[4]
        m = re.search(r"(sub_[0-9A-Fa-f]{8})", fname or "")
        if m and m.group(1) in band_aid_syms:
            shipping_band_aids.append(site)
    if defon:
        print(f"NOTE: {len(defon)} site(s) are gated by a private SR_* env that "
              f"DEFAULTS ON, i.e. active in the default exe. Platform fixes are "
              f"expected here; anything else should be master-gated or default-OFF.")
    if shipping_band_aids:
        dump("BAND-AID SHIPPING BY DEFAULT (registry-classified, default-ON)",
             shipping_band_aids)
        print(f"RESULT: {len(shipping_band_aids)} band-aid site(s) are ACTIVE IN THE "
              f"DEFAULT EXE. Put them behind forcingAllowed() or default the gate OFF.")
        failed.append(f"{len(shipping_band_aids)} band-aid default-ON")

    # iter-374: hand edits in GENERATED guest code (see scan_generated). This is a
    # separate pass because guest/ppc is gitignored and was never audited at all —
    # which is how an ungated fabricator survived in every build while this script
    # printed "forces nothing".
    writers = generated_call_writers(sites)
    gen_sites, gen_ungated = scan_generated(ROOT, ctx=LAST_CTX.get("ctx"), writers=writers)
    if gen_sites:
        print(f"\n--- HAND EDITS IN GENERATED guest/ppc ({len(gen_sites)}; "
              f"{len(gen_ungated)} ungated) ---")
        for path, line, sym, gated in gen_sites:
            print(f"  {'GATED  ' if gated else 'UNGATED'} "
                  f"{os.path.relpath(path, ROOT)}:{line}  sr::kernel::{sym}")
    if gen_ungated:
        print(f"\nRESULT: {len(gen_ungated)} UNGATED hand edit(s) in generated guest "
              f"code. NOT a clean forcing-off default.")
        failed.append(f"{len(gen_ungated)} generated")
    # iter-463 round 2: the same check over the COMMITTED splice bodies (what
    # tools/regen/assemble.py writes into guest/ppc), so it holds on a checkout
    # without the gitignored guest/ppc too.
    sp_sites, sp_ungated = scan_generated(ROOT, "splice", ctx=LAST_CTX.get("ctx"), writers=writers)
    if os.path.exists(os.path.join(ROOT, SPLICE_BODIES)):
        print(f"\n--- HAND EDITS IN SPLICE BODIES ({SPLICE_BODIES}: {len(sp_sites)}; "
              f"{len(sp_ungated)} ungated) ---")
    else:
        print(f"\n--- SPLICE BODIES: none (R1: guest/ppc is the recompiler output plus hooks) ---")
    for path, line, sym in sp_ungated:
        print(f"  UNGATED {path}:{line}  sr::kernel::{sym}")
    if sp_ungated:
        print(f"\nRESULT: {len(sp_ungated)} UNGATED hand edit(s) in splice bodies. "
              f"NOT a clean forcing-off default.")
        failed.append(f"{len(sp_ungated)} splice")
    # iter-463 burndown-2 review: what the pass above cannot see (forced exits,
    # deleted generated code) - ratcheted, so the number can only go down.
    sd_n, sd_base = (splice_divergence_report(ROOT, LAST_CTX.get("ctx"))
                     if os.path.exists(os.path.join(ROOT, SPLICE_BODIES)) else (None, None))
    if sd_n is not None and sd_base is not None and sd_n > sd_base:
        print(f"\nRESULT: {sd_n} unreviewed splice divergence(s) > baseline {sd_base} "
              f"({SPLICE_DIV_BASELINE}). A new forced exit / dropped generated code "
              f"landed in a splice body: gate it behind forcingAllowed() or justify it.")
        failed.append(f"{sd_n} splice divergence(s) > baseline {sd_base}")
    # iter-463 review: the committed splice bodies must match what is DEPLOYED in
    # guest/ppc, or the next regen silently reverts the difference (found: the
    # HlslCompileShader restore, the Lua GC sweep root fix, SR_G2_RENDERWORK).
    if os.path.isdir(os.path.join(ROOT, GENERATED_DIR)) and os.path.isfile(
            os.path.join(ROOT, "tools", "regen", "check_splice_sync.py")):
        sys.path.insert(0, os.path.join(ROOT, "tools", "regen"))
        try:
            import check_splice_sync as _css
            _raw = open(_css.SPLICE, "rb").read()
            _b = json.loads(_raw.decode("utf-8"))
            _want = {n for n, v in _b.items() if isinstance(v, str)}
            _dep = _css.parse_bodies(_css.DEPLOYED, _want)
            _drift = sorted(n for n in _want & set(_dep) if _css.norm(_b[n]) != _css.norm(_dep[n]))
            print(f"\n--- SPLICE BODIES vs DEPLOYED guest/ppc: {len(_drift)} drifted "
                  f"(tools/regen/check_splice_sync.py) ---")
            for n in _drift:
                print(f"  DRIFT {n}")
            if _drift:
                print(f"\nRESULT: {len(_drift)} splice body(ies) differ from the deployed code - "
                      f"a regen would revert them. python tools/regen/check_splice_sync.py --write")
                failed.append(f"{len(_drift)} splice drift")
        except Exception as e:  # the check must never crash the audit
            print(f"\n--- SPLICE BODIES vs DEPLOYED guest/ppc: check failed ({e!r}) ---")

    # iter-389 OVERRIDE REGISTRY RATCHET: every kernel strong-def override of a
    # recompiled guest fn must be classified in docs/OVERRIDES.json (generated
    # by scripts/gen_override_manifest.py, manual fields persist). An override
    # NOT in the manifest fails the audit — new behavior can no longer land
    # invisibly. Entries still 'needs-review' are listed (warning) until the
    # burndown finishes; then this flips to a failure too.
    import json as _json
    import glob
    manifest_path = os.path.join(ROOT, "docs", "OVERRIDES.json")
    ov_re = re.compile(
        r"^(?:void\s+(sub_[0-9A-Fa-f]{8})\s*\(\s*PPCContext|PPC_FUNC\((sub_[0-9A-Fa-f]{8})\))",
        re.M)
    defined = {}
    # iter-463 round 4 (D16): the SAME file set the registry generator scans -
    # kernel/*.cpp, guest/*.cpp and the hand-written guest stubs - and its macro
    # resolution (PPC_MISSING_STUB(sub_X)). Round 3 checked kernel/*.cpp only.
    try:
        import gen_override_manifest as _gom
        defined = {s: e["file"] for s, e in _gom.scan().items()}
    except Exception as ex:                      # noqa: BLE001 - fall back, loudly
        print(f"NOTE: registry scan via gen_override_manifest failed ({ex}); kernel/ only")
        for path in sorted(glob.glob(os.path.join(ROOT, "kernel", "*.cpp"))):
            with open(path, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
            for m in ov_re.finditer(text):
                defined[m.group(1) or m.group(2)] = os.path.basename(path)
    manifest = {}
    if os.path.exists(manifest_path):
        with open(manifest_path, encoding="utf-8") as fh:
            manifest = _json.load(fh)
    unregistered = sorted(s for s in defined if s not in manifest)
    needs_review = sorted(s for s, e in manifest.items()
                          if e.get("class") == "needs-review")
    bandaids = sorted(s for s, e in manifest.items()
                      if e.get("class") == "band-aid" and not e.get("retire"))
    if needs_review:
        print(f"\nOVERRIDE REGISTRY: {len(needs_review)} needs-review "
              f"(burndown pending): {', '.join(needs_review[:10])}"
              f"{'...' if len(needs_review) > 10 else ''}")
    if bandaids:
        print(f"OVERRIDE REGISTRY: {len(bandaids)} band-aid(s) MISSING a "
              f"retirement condition: {', '.join(bandaids)}")
    if unregistered:
        print(f"\nRESULT: {len(unregistered)} kernel override(s) NOT in "
              f"docs/OVERRIDES.json: {', '.join(unregistered[:10])}"
              f"{'...' if len(unregistered) > 10 else ''}\n"
              f"Run scripts/gen_override_manifest.py and classify them.")
        failed.append(f"{len(unregistered)} unregistered override(s)")

    # iter-455l r4: the isolation choke point is part of the standard audit — a
    # violation here means SR_FORCING_ISOLATE could be bypassed, which invalidates
    # any treatment run that relied on it.
    # iter-463 phase 2: the dev-tool class's hard rules (DEVTOOLS_DESIGN 2.1).
    dt_violations = devtool_report(parsed, ROOT, sites)
    if dt_violations:
        print(f"\nRESULT: {len(dt_violations)} dev-tool rule violation(s).")
        failed.append(f"{len(dt_violations)} devtool")

    iso_violations = isolation_report(parsed)
    if iso_violations:
        print(f"\nRESULT: {len(iso_violations)} isolation choke-point violation(s). "
              f"SR_FORCING_ISOLATE can be BYPASSED — fix before any isolated run.")
        failed.append(f"{len(iso_violations)} isolation")

    if failed:
        print(f"\nRESULT: FAIL ({'; '.join(failed)}).")
        return 1

    # This line used to end "Default exe forces nothing (by this check)", which
    # read as an all-clear while 83 own-gated sites sat in the same report. It
    # now states what was actually verified and carries the counts that qualify
    # it, so a passing run can never again be quoted as "no forcing".
    print("RESULT: no UNGATED, non-allowlisted forcing sites, and no "
          "registry-classified band-aid active by default in kernel write sites.")
    print(f"        BUT {hk_n} band-aid hook(s) on generated code ship in the default exe (live "
          f"{hk_by.get('live', 0)}, conditional {hk_by.get('conditional', 0)}): the forcing number is "
          f"{len(flagged) + hk_n}, not 0 (HOOKS ON GENERATED CODE above; RELEASE_PLAN phase 4).")
    print(f"        Qualifies as: {counts['OWNGATE_ON']} default-ON own-gated site(s) "
          f"and {counts['OWNGATE']} opt-in own-gated site(s) are NOT master-gated; "
          f"they are declared, not absent.")
    return 0


def host_launch_audit(mode):
    """iter-463: the forcing surface this file cannot see -- host threads / timers /
    VEH / atexit callbacks that write guest memory, call guest code or signal a
    guest-visible object on their own (cfgSoundOffThread zeroed the Sound flag and
    the MODGLOB poller armed on every natural roll while this audit printed "no
    UNGATED"). Checked by scripts/audit_host_threads.py against the registry
    docs/HOST_THREADS.json (platform / diagnostic entries only). Runs in every mode
    except --json / --isolation; under --count its report goes to stderr so the
    printed number stays machine-readable."""
    if mode in ("--json", "--isolation"):
        return 0
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import audit_host_threads
    print()
    return audit_host_threads.run(ROOT, "--all" if mode == "--all" else "--summary",
                                  out=sys.stderr if mode == "--count" else sys.stdout)


if __name__ == "__main__":
    _mode = sys.argv[1] if len(sys.argv) > 1 else "--summary"
    _rc = main()
    sys.exit(host_launch_audit(_mode) or _rc)
