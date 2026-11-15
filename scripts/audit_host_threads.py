#!/usr/bin/env python3
"""
audit_host_threads.py -- the HOST-LAUNCH half of the forcing audit (iter-463).

audit_forcing.py classifies guest-state primitives by the gate around each call
site, and docs/OVERRIDES.json registers strong-def overrides. Neither sees code
the HOST starts on its own: a std::thread / CreateThread / timer / VEH / atexit
callback whose body writes guest memory, signals a guest-visible kernel object
or calls guest code. Two such threads ran ungated on every natural roll
(cfgSoundOffThread zeroing the Sound config flag; the MODGLOB poller) while
audit_forcing.py printed "no UNGATED". This check closes that hole.

For every host launch site in kernel/, gfx/, app/ and guest/*.cpp (not the
generated guest/ppc) it:
  1. resolves the launched body (lambda, or the named entry function) and follows
     calls to functions defined in the same file (depth 3), skipping bodies of
     launches nested inside it (those are audited as their own sites);
  2. looks for GUEST-TOUCHING primitives: guest writes (writeGuest*, local
     writeBE32 helpers, PPC_STORE_*, raw stores / memcpy / memset through a
     pointer derived from the guest base), guest calls (GuestCaller, __imp__sub_,
     fn(ctx, base), PPC_LOOKUP_FUNC), guest-heap allocation, guest-visible
     kernel-object signals (ReleaseSemaphore, SetEvent, stub_Ke*/Nt* signals),
     XNotify enqueues, and dispatch-table patches;
  3. passes the site only if the LAUNCH is master-gated -- inside an
     `if (...forcingAllowed()...)` branch (no `||` bypass), or after an
     `if (!forcingAllowed() ...) return` guard in an enclosing block, or through
     a predicate whose own definition requires forcingAllowed() -- OR the site
     is listed in docs/HOST_THREADS.json "allow" with a class (platform /
     diagnostic) and a reason. An allow entry may name `must_gate` predicates:
     a forcing branch INSIDE an allowed thread; each must require forcingAllowed().
     It may name `requires` regexes that keep an observe claim checkable (e.g.
     `constexpr bool kObserveOnly = true`); if one disappears the site fails.
A body gate (thread starts, then returns unless forcingAllowed()) does NOT pass:
the charter wants a default run to start nothing, so the gate goes at the launch.

docs/HOST_THREADS.json "open_forcing" is the register of forcing verdicts
still open. "launch" entries annotate failing launch sites with the verdict's
recommendation; "site" entries (forcing that is not a host launch, e.g. a
harness write inside a guest call) FAIL while their anchor exists un-gated.
The register is not an allowlist: removing an entry never clears a launch
failure, only fixing the code does.

docs/HOST_THREADS.json "declared" holds DECLARED BAND-AIDS: forcing launches
that must stay live for now because removing them exposes a platform gap not
yet fixed. Each entry needs id, file, entry, a reason, a `retirement`
condition and a `bandaids` pointer (docs/BANDAIDS.md). A matching launch
reports DECLARED and passes; the summary always prints the declared list so
the band-aid stays visible. It is the ONLY non-platform/observe way to pass.

HEURISTIC LIMITS: same-file call following only; raw-store detection keys on
pointers built from a guest-base token (gb/base/gbase/g_guestBase/guestBase/
guestVAToHost); a gate expressed through anything other than the forms above
reads as ungated (safe direction). Usage:
    python scripts/audit_host_threads.py [--all|--json]
Exit 0 = every guest-touching host launch is master-gated or allowlisted and no
open_forcing site remains; 1 otherwise.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGISTRY = os.path.join("docs", "HOST_THREADS.json")

# ---------------------------------------------------------------- sources


def source_files(root):
    out = []
    for d in ("kernel", "gfx", "app"):
        base = os.path.join(root, d)
        for r, _, files in os.walk(base):
            for fn in files:
                if fn.endswith((".cpp", ".cc", ".h", ".hpp")):
                    out.append(os.path.join(r, fn))
    gdir = os.path.join(root, "guest")
    if os.path.isdir(gdir):
        for fn in os.listdir(gdir):                      # top level only: guest/ppc is generated
            if fn.endswith((".cpp", ".cc", ".h", ".hpp")):
                out.append(os.path.join(gdir, fn))
    return sorted(out)


_STRIP_CACHE = {}


def strip_code(text):
    """Blank comments and string/char literal CONTENTS (same length, newlines kept).
    (Memoized: audit_forcing strips each file several times per run, and the
    test suite re-audits the same tree many times.)"""
    hit = _STRIP_CACHE.get(text)
    if hit is not None:
        return hit
    res = _strip_code(text)
    if len(_STRIP_CACHE) > 4096:
        _STRIP_CACHE.clear()
    _STRIP_CACHE[text] = res
    return res


def _strip_code(text):
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c == 'R' and text[i:i + 2] == 'R"':            # raw string R"delim( ... )delim"
            m = re.match(r'R"([^(\s]*)\(', text[i:i + 20])
            if m:
                end = text.find(")" + m.group(1) + '"', i)
                end = n if end < 0 else end + len(m.group(1)) + 2
                for k in range(i + 2, end - 1):
                    if out[k] != "\n":
                        out[k] = " "
                i = end
            else:
                i += 1
        elif c in "\"'":
            if c == "'" and i > 0 and (text[i - 1].isalnum()) and text[i - 1] not in "uULR8":
                i += 1                                     # digit separator 1'000
                continue
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out)


class Src:
    def __init__(self, path, root):
        self.path = path
        self.rel = os.path.relpath(path, root).replace("\\", "/")
        self.raw = open(path, encoding="utf-8", errors="replace").read()
        self.clean = strip_code(self.raw)
        self._line_starts = [0] + [m.end() for m in re.finditer("\n", self.raw)]
        self._match_braces()
        self._index_functions()

    def line_of(self, pos):
        lo, hi = 0, len(self._line_starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self._line_starts[mid] <= pos:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    def _match_braces(self):
        self.brace_match = {}
        self.parent = {}                                  # '{' pos -> enclosing '{' pos (or -1)
        stack = []
        for i, ch in enumerate(self.clean):
            if ch == "{":
                self.parent[i] = stack[-1] if stack else -1
                stack.append(i)
            elif ch == "}" and stack:
                o = stack.pop()
                self.brace_match[o] = i
        self.opens = sorted(self.brace_match)

    def enclosing(self, pos):
        """Innermost '{' whose block contains pos, then its ancestors (list)."""
        best = -1
        # linear scan is fine at these file sizes, but bisect over opens first
        import bisect
        k = bisect.bisect_right(self.opens, pos) - 1
        while k >= 0:
            o = self.opens[k]
            if self.brace_match[o] >= pos:
                best = o
                break
            k -= 1
        chain = []
        while best != -1:
            chain.append(best)
            best = self.parent.get(best, -1)
        return chain

    def innermost(self, pos):
        ch = self.enclosing(pos)
        return ch[0] if ch else -1

    _KW = {"if", "for", "while", "switch", "catch", "return", "sizeof", "defined",
           "alignof", "decltype", "static_assert", "else", "do", "case", "new", "delete"}

    def _index_functions(self):
        """name -> [(open_brace, close_brace)] for function-like definitions."""
        self.funcs = {}
        self.sigs = {}
        rx = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
        for m in rx.finditer(self.clean):
            name = m.group(1)
            if name in self._KW:
                continue
            p = self._match_paren(m.end() - 1)
            if p is None:
                continue
            tail = self.clean[p + 1:p + 200]
            t = re.match(r"\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:->\s*[\w:<>\*&\s]+?)?\s*\{", tail)
            if not t:
                continue
            ob = p + 1 + t.end() - 1
            if ob not in self.brace_match:
                continue
            # reject control statements / calls followed by a block (e.g. `x(y) {` in macros is rare)
            before = self.clean[max(0, m.start() - 40):m.start()]
            if re.search(r"(?:\bif|\bwhile|\bfor|\bswitch|=|\breturn)\s*$", before):
                continue
            self.funcs.setdefault(name, []).append((ob, self.brace_match[ob]))
            self.sigs[(name, ob)] = self.clean[max(0, m.start() - 60):m.start()]
        # local lambdas `auto name = [caps](params) { ... }` are callable by name too
        for m in re.finditer(r"\bauto\s+([A-Za-z_]\w*)\s*=\s*\[", self.clean):
            ob = self.clean.find("{", m.end())
            if ob in self.brace_match and ";" not in self.clean[m.end():ob]:
                self.funcs.setdefault(m.group(1), []).append((ob, self.brace_match[ob]))

    def _match_paren(self, open_pos):
        depth = 0
        for i in range(open_pos, len(self.clean)):
            c = self.clean[i]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    return i
        return None


# ---------------------------------------------------------------- launches

LAUNCH_RX = [
    # (kind, regex matching up to and including the call's '(' or '{', arg index of the entry)
    ("std::thread", re.compile(r"\bstd::j?thread\b(?!\s*::)(?:\s+\w+)?\s*[({]"), 0),
    ("CreateThread", re.compile(r"\bCreateThread\s*\("), 2),
    ("_beginthreadex", re.compile(r"\b_beginthread(?:ex)?\s*\("), 2),
    ("CreateTimerQueueTimer", re.compile(r"\bCreateTimerQueueTimer\s*\("), 2),
    ("SetTimer", re.compile(r"(?<![\w:.>])SetTimer\s*\("), 3),
    ("timeSetEvent", re.compile(r"\btimeSetEvent\s*\("), 2),
    ("CreateThreadpoolTimer", re.compile(r"\bCreateThreadpool(?:Timer|Work|Wait)\s*\("), 0),
    ("TrySubmitThreadpoolCallback", re.compile(r"\bTrySubmitThreadpoolCallback\s*\("), 0),
    ("QueueUserAPC", re.compile(r"\bQueueUserAPC\s*\("), 0),
    ("VEH", re.compile(r"\bAddVectored(?:Exception|Continue)Handler\s*\("), 1),
    ("UnhandledExceptionFilter", re.compile(r"\bSetUnhandledExceptionFilter\s*\("), 0),
    ("atexit", re.compile(r"(?<![\w.>])(?:std::)?(?:atexit|at_quick_exit)\s*\("), 0),
    ("SDL_AddTimer", re.compile(r"\bSDL_AddTimer\s*\("), 1),
    ("SDL_CreateThread", re.compile(r"\bSDL_CreateThread\s*\("), 0),
    ("std::async", re.compile(r"\bstd::async\s*\("), 0),
]


def split_args(s):
    args, depth, cur = [], 0, []
    for ch in s:
        if ch in "([{<" and not (ch == "<" and False):
            if ch != "<":
                depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            args.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        args.append("".join(cur))
    return args


class Launch:
    def __init__(self, src, kind, pos, stmt_end, entry_name, body_ranges, arg_span):
        self.src = src
        self.kind = kind
        self.pos = pos
        self.line = src.line_of(pos)
        self.stmt_end = stmt_end
        self.entry = entry_name               # function name, or "lambda"
        self.body_ranges = body_ranges        # [(start,end)] of the launched code in src
        self.arg_span = arg_span              # (start,end) of the launch call's argument text
        self.prims = []                       # [(kind, line, text)]
        self.gate = None                      # None or description


def find_launches(src):
    out = []
    for kind, rx, argi in LAUNCH_RX:
        for m in rx.finditer(src.clean):
            opener = m.end() - 1
            closer_ch = ")" if src.clean[opener] == "(" else "}"
            if closer_ch == ")":
                close = src._match_paren(opener)
            else:
                close = src.brace_match.get(opener)
            if close is None:
                continue
            args_txt = src.clean[opener + 1:close]
            args = split_args(args_txt)
            if kind == "std::async" and args and "launch::" in args[0]:
                argi_eff = 1
            else:
                argi_eff = argi
            if kind == "std::thread" and not args_txt.strip():
                continue                                   # default-constructed
            if argi_eff >= len(args):
                continue
            # absolute offset of the entry argument
            off = opener + 1
            for a in args[:argi_eff]:
                off += len(a) + 1
            arg = args[argi_eff]
            arg_start = off
            a = arg.strip()
            entry, ranges = None, []
            if a.startswith("["):
                ob = src.clean.find("{", arg_start)
                if ob != -1 and ob in src.brace_match and ob < close:
                    ranges = [(ob, src.brace_match[ob])]
                entry = "lambda"
            else:
                name = re.sub(r"^[&\s]*", "", a)
                name = re.sub(r"^\(\s*\w+\s*\)\s*", "", name)          # (LPTHREAD_START_ROUTINE)fn
                name = re.sub(r"^reinterpret_cast<[^>]*>\((.*)\)$", r"\1", name).strip()
                name = name.split("::")[-1].strip("&* ")
                if re.fullmatch(r"[A-Za-z_]\w*", name or ""):
                    entry = name
                    ranges = list(src.funcs.get(name, []))
                    if not ranges:
                        # a local lambda variable: `auto name = [..](..) { .. };`
                        lv = None
                        for lm in re.finditer(r"\b" + re.escape(name) + r"\s*=\s*\[", src.clean[:m.start()]):
                            lv = lm
                        if lv:
                            ob = src.clean.find("{", lv.end())
                            if ob in src.brace_match:
                                ranges = [(ob, src.brace_match[ob])]
            out.append(Launch(src, kind, m.start(), close, entry or a[:40], ranges,
                              (opener + 1, close)))
    out.sort(key=lambda l: l.pos)
    return out


# ---------------------------------------------------------------- primitives

GUEST_BASE_TOK = r"(?:\bgb\b|\bbase\b|\bgbase\b|\bg_guestBase\b|\bguestBase\b|\bgBase\b|\bguestVAToHost\s*\(|\bgetGuestPtr\s*\()"
PRIMS = [
    ("guest_write", re.compile(r"\bwriteGuest\w*\s*\(|\bwrite(?:BE|Be|U)\d*\w*\s*\(|\bstoreBE\w*\s*\(|\bwrite32BE\s*\(")),
    ("ppc_store", re.compile(r"\bPPC_STORE_\w+\s*\(")),
    ("guest_call", re.compile(r"\bGuestCaller\b|\b__imp__sub_[0-9A-Fa-f]{8}\s*\(|"
                              r"\bsub_[0-9A-Fa-f]{8}\s*\(\s*\w+\s*,\s*\w+\s*\)|"
                              r"\bPPC_LOOKUP_FUNC(?:_SAFE)?\s*\([^;]*\)\s*\(|"
                              r"\b\w+\s*\(\s*ctx\s*,\s*(?:base|gb|gbase|guestBase|g_guestBase|gBase)\s*\)")),
    ("guest_heap", re.compile(r"\bguestHeapAlloc\w*\s*\(")),
    ("kobj_signal", re.compile(r"(?<![\w:])(?:ReleaseSemaphore|SetEvent|PulseEvent)\s*\(|"
                               r"\bstub_(?:Ke|Nt)\w*(?:Set|Release|Pulse)\w*\s*\(|\bKe(?:SetEvent|ReleaseSemaphore)\s*\(")),
    ("xnotify", re.compile(r"\bxnEnqueue\s*\(")),
    ("table_patch", re.compile(r"\bPPC_LOOKUP_FUNC\s*\([^;]*\)\s*=(?!=)|\bg_dispatchTable\s*\[[^\]]*\]\s*=(?!=)")),
    ("raw_memwrite", re.compile(r"\bmem(?:cpy|set|move)\s*\(\s*\(?\s*(?:reinterpret_cast<[^>]*>\s*\()?\s*" + GUEST_BASE_TOK + r"\s*\+")),
]
DERIVED_DECL = re.compile(r"(?<![\w])(?:uint8_t|uint16_t|uint32_t|uint64_t|char|void|auto|int8_t|int32_t|be<\w+>|float)"
                          r"\s*\*\s*(?:const\s+)?(\w+)\s*=\s*([^;]*)")
NESTED_LAUNCH = re.compile("|".join("(?:%s)" % rx.pattern for _, rx, _ in LAUNCH_RX))
CALL_RX = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


def derived_vars(t):
    """Names of NON-const pointers built from a guest-base token in text t."""
    derived = set()
    for m in DERIVED_DECL.finditer(t):
        decl_prefix = t[max(0, m.start() - 12):m.start()]
        if re.search(r"\bconst\s*$", decl_prefix):
            continue
        if re.search(GUEST_BASE_TOK, m.group(2)):
            derived.add(m.group(1))
    return derived


def scan_region(src, start, end, skip_spans, extra_derived=()):
    """Primitive hits in src.clean[start:end] excluding skip spans."""
    text = list(src.clean[start:end + 1])
    for a, b in skip_spans:
        for k in range(max(a, start), min(b, end) + 1):
            if text[k - start] != "\n":
                text[k - start] = " "
    t = "".join(text)
    hits = []
    for kind, rx in PRIMS:
        for m in rx.finditer(t):
            hits.append((kind, start + m.start()))
    # raw stores through a guest-derived NON-const pointer
    derived = derived_vars(t) | set(extra_derived)
    for v in derived:
        vr = re.escape(v)
        # store forms: v[i] = x   |   *v = x / *(T*)v = x (a deref at statement start,
        # so the declaration `uint8_t* v = gb + ..` itself is not a store)   |   memcpy(v, ..)
        rx = re.compile(r"\b" + vr + r"\s*\[[^\]]*\]\s*=(?!=)|(?:^|(?<=[;{}]))\s*\*\s*(?:\([^()]*\)\s*)?" + vr +
                        r"\s*=(?!=)|\bmem(?:cpy|set|move)\s*\(\s*" + vr + r"\b", re.M)
        for m in rx.finditer(t):
            hits.append(("raw_store", start + m.start()))
    return hits, t


def analyze(launch, all_launch_spans):
    src = launch.src
    # pointers the launch site's own function built from the guest base and hands to
    # a lambda by capture (`uint8_t* bundle = g_guestBase + VA; std::thread([bundle]{..})`)
    chain = src.enclosing(launch.pos)
    outer = chain[-1] if chain else -1
    captured = (derived_vars(src.clean[outer + 1:launch.pos])
                if outer != -1 and launch.entry == "lambda" else set())
    seen, work, hits = set(), [(r, 0) for r in launch.body_ranges], []
    while work:
        (s, e), depth = work.pop()
        if (s, e) in seen:
            continue
        seen.add((s, e))
        skips = [sp for sp in all_launch_spans if s < sp[0] and sp[1] <= e]
        h, t = scan_region(src, s, e, skips, captured)
        hits += h
        if depth >= 3:
            continue
        for m in CALL_RX.finditer(t):
            name = m.group(1)
            for fr in src.funcs.get(name, []):
                if not (fr[0] <= s + m.start() <= fr[1]):   # not recursion into itself
                    work.append((fr, depth + 1))
    uniq = {}
    for kind, pos in hits:
        uniq.setdefault((kind, src.line_of(pos)), pos)
    launch.prims = sorted(((k, ln, src.raw.split("\n")[ln - 1].strip()[:110]) for (k, ln) in uniq),
                          key=lambda x: x[1])


# ---------------------------------------------------------------- gates

OR_BYPASS = re.compile(r"\|\|")
# Switch ONLY for scripts/test_audit_forcing.py --mutate (proves the B1 case in
# scripts/test_audit_host_threads.py catches the bug); always on otherwise.
B1_FIX = True
# iter-463 round 4 (verifier item 8), same mutation-only contract:
#   `} else if (!forcingAllowed()) return;` read as an early-return guard (the
#   launch runs whenever the first branch is taken), and
#   `if (forcingAllowed() == false) { launch }` read as master-gated.
H2_FIX = True


def master_tokens(srcs):
    """Gate predicates whose OWN definition requires forcingAllowed() (no || bypass).

    Returns (global_fns, {rel: file_vars}): bool-returning functions are global;
    bool variables (`static const bool s_x = forcingAllowed() && ..`) are per file."""
    # B1 (iter-463): predicates are matched by bare name, so a name is trusted as
    # a master gate only when EVERY bool definition of it in the tree requires
    # forcingAllowed(). A same-named own-gate elsewhere (a::gate() master,
    # b::gate() plain) would otherwise make `if (b::gate())` read master-gated.
    good, bad, fvars = set(), set(), {}
    for s in srcs:
        for name, ranges in s.funcs.items():
            for ob, cb in ranges:
                sig = s.sigs.get((name, ob), "")
                if not re.search(r"\bbool\s+(?:[\w:]+::)?$", sig):
                    continue
                body = s.clean[ob:cb + 1]
                if "forcingAllowed" in body and len(body) < 4000:
                    (good if requires_master_body(body) else bad).add(name)
                elif B1_FIX:
                    bad.add(name)
        vs = set()
        for m in re.finditer(r"\bbool\s+(\w+)\s*=\s*", s.clean):
            init = s.clean[m.end():m.end() + 800]
            d, j = 0, 0
            for j, ch in enumerate(init):
                if ch in "{(":
                    d += 1
                elif ch in "})":
                    d -= 1
                elif ch == ";" and d <= 0:
                    break
            init = init[:j]
            if "forcingAllowed" in init and not OR_BYPASS.search(init):
                vs.add(m.group(1))
        fvars[s.rel] = vs
    # iter-463 burndown2: forcingAllowedFor("NAME") requires the master gate too (guest_context.cpp
    # isolationAllowsNamed: master off -> false); audit_forcing.py's isolation report restricts NAME to
    # the reviewed ISOLATION_EXCEPTIONS literals.
    return (good - bad) | {"forcingAllowed", "forcingAllowedFor"}, fvars


def requires_master_body(body):
    """A predicate body 'requires' the master gate if it returns false/early without it
    and never ORs it with something else."""
    if OR_BYPASS.search(re.sub(r"if\s*\(\s*![^)]*forcingAllowed[^)]*\)\s*\)\s*return", "", body)):
        # an || somewhere: accept only the `if (!forcingAllowed() || ...) return false` guard form
        if not re.search(r"if\s*\(\s*!\s*(?:sr::kernel::)?forcingAllowed\s*\(\s*\)[^;{]*\)\s*return\s+false", body):
            return False
    return bool(re.search(r"!\s*(?:sr::kernel::)?forcingAllowed\s*\(\s*\)|return\s+[^;]*\bforcingAllowed\s*\(\s*\)\s*&&|"
                          r"&&\s*(?:sr::kernel::)?forcingAllowed\s*\(\s*\)|return\s+(?:sr::kernel::)?forcingAllowed\s*\(\s*\)\s*;", body))


def _normalize_cmp(cond, toks):
    """H2: `M() == false` / `M() == 0` / `M() != true` -> `!M()`, and
    `M() == true` / `M() != false` / `M() != 0` -> `M()`."""
    call = r"(" + toks + r"(?:\s*\(\s*\))?)"
    cond = re.sub(call + r"\s*(?:==\s*(?:false|0)|!=\s*true)\b", r"!\1", cond)
    cond = re.sub(r"\b(?:false|0)\s*==\s*" + call, r"!\1", cond)
    cond = re.sub(call + r"\s*(?:==\s*true|!=\s*(?:false|0))\b", r"\1", cond)
    return cond


def cond_requires(cond, masters, negated_guard=False):
    toks = r"(?:sr::kernel::)?(?:%s)\b" % "|".join(sorted(map(re.escape, masters), key=len, reverse=True))
    if H2_FIX:
        cond = _normalize_cmp(cond, toks)
    if negated_guard:
        # `if (!M || other) return` guards; `&&` would let a non-master path through
        return bool(re.search(r"!\s*" + toks, cond)) and "&&" not in cond
    if "||" in cond:
        return False
    for m in re.finditer(toks, cond):
        pre = cond[:m.start()].rstrip()
        if not pre.endswith("!"):
            return True
    return False


def gate_of(src, pos, masters):
    """Why the code at pos only runs with the master gate on (or None)."""
    chain = src.enclosing(pos)
    # (a) statement-level `if (COND) <stmt containing pos>` without braces
    k = pos - 1
    while k >= 0 and src.clean[k] not in ";{}":
        k -= 1
    stmt = src.clean[k + 1:pos]
    m = re.match(r"\s*(?:else\s+)?if\s*\(", stmt)
    if m:
        p = src._match_paren(k + 1 + m.end() - 1)
        if p is not None and p < pos and cond_requires(src.clean[k + 1 + m.end():p], masters):
            return "if-statement"
    # (b) enclosing `if (COND) {` blocks
    for ob in chain:
        k = ob - 1
        while k >= 0 and src.clean[k].isspace():
            k -= 1
        if k >= 0 and src.clean[k] == ")":
            # walk back to the matching '('
            depth, j = 0, k
            while j >= 0:
                if src.clean[j] == ")":
                    depth += 1
                elif src.clean[j] == "(":
                    depth -= 1
                    if depth == 0:
                        break
                j -= 1
            head = src.clean[max(0, j - 12):j]
            if re.search(r"\bif\s*$", head) and cond_requires(src.clean[j + 1:k], masters):
                return "if-block"
    # (c) an early-return guard `if (!M ...) return` in an enclosing block before pos
    for ob in chain:
        seg = src.clean[ob + 1:pos]
        for m in re.finditer(r"\bif\s*\(", seg):
            a = ob + 1 + m.end() - 1
            if src.innermost(a) != ob:
                continue
            p = src._match_paren(a)
            if p is None or p > pos:
                continue
            if H2_FIX and re.search(r"\belse\s*$", seg[:m.start()]):
                continue                  # `else if (!M) return;` guards one branch only
            after = src.clean[p + 1:p + 40]
            if re.match(r"\s*\{?\s*return\b", after) and cond_requires(src.clean[a + 1:p], masters, True):
                return "early-return guard"
    return None


# ---------------------------------------------------------------- registry


def load_registry(root):
    p = os.path.join(root, REGISTRY)
    if not os.path.exists(p):
        return {"allow": [], "open_forcing": []}
    with open(p, encoding="utf-8") as fh:
        return json.load(fh)


def launch_text(launch):
    s = launch.src
    parts = [s.raw[launch.pos:launch.stmt_end + 1]]
    for a, b in launch.body_ranges:
        parts.append(s.raw[a:b + 1])
    return "\n".join(parts)


def entry_matches(ent, launch):
    if ent.get("file") != launch.src.rel:
        return False
    want = ent.get("entry")
    if want and launch.entry not in (want if isinstance(want, list) else [want]):
        return False
    anc = ent.get("anchor")
    if anc and anc not in launch_text(launch):
        return False
    return True


# ---------------------------------------------------------------- main


def run(root=ROOT, mode="--summary", out=sys.stdout):
    srcs = [Src(p, root) for p in source_files(root)]
    gmasters, fvars = master_tokens(srcs)
    mfor = lambda rel: gmasters | fvars.get(rel, set())
    reg = load_registry(root)
    allow = [e for e in reg.get("allow", []) if e.get("class") in ("platform", "diagnostic")]
    bad_cls = [e for e in reg.get("allow", []) if e.get("class") not in ("platform", "diagnostic")
               or not e.get("reason")]
    # iter-463 phase 2 (docs/DEVTOOLS_DESIGN.md 2.2): the `devtool` class MUST stay empty - every
    # dev-tool action runs on the game's own tick thread, so any row here fails like a bad class.
    bad_cls += [dict(e, **{"class": "devtool (this list must stay EMPTY)"}) if isinstance(e, dict)
                else {"file": str(e), "class": "devtool (this list must stay EMPTY)"}
                for e in reg.get("devtool", [])]
    open_forcing = reg.get("open_forcing", [])
    declared = [e for e in reg.get("declared", [])
                if e.get("reason") and e.get("retirement") and e.get("bandaids")]
    bad_decl = [e for e in reg.get("declared", []) if e not in declared]

    launches = []
    for s in srcs:
        ls = find_launches(s)
        spans = [l.arg_span for l in ls]
        for l in ls:
            analyze(l, [sp for sp in spans if sp != l.arg_span])
            l.gate = gate_of(s, l.pos, mfor(s.rel))
        launches += ls

    rows = []          # (status, launch, allow_entry or None, note)
    used_allow = set()
    for l in launches:
        if not l.prims:
            rows.append(("INERT", l, None, "no guest-touching primitive in the launched body"))
            continue
        if l.gate:
            rows.append(("MASTER", l, None, l.gate))
            continue
        ent = next((e for e in allow if entry_matches(e, l)), None)
        if ent:
            used_allow.add(id(ent))
            ungated = [g for g in ent.get("must_gate", []) if g not in mfor(l.src.rel)]
            missing = [r for r in ent.get("requires", []) if not re.search(r, launch_text(l))]
            if ungated:
                rows.append(("FAIL", l, ent, "allowlisted, but its forcing branch predicate(s) "
                             + ", ".join(ungated) + " do not require forcingAllowed()"))
            elif missing:
                rows.append(("FAIL", l, ent, "allowlisted as observe, but the pattern(s) that made it "
                             "observe-only are gone: " + ", ".join(missing)))
            else:
                rows.append(("ALLOW", l, ent, ent["class"]))
            continue
        dent = next((e for e in declared if entry_matches(e, l)), None)
        if dent:
            used_allow.add(id(dent))
            rows.append(("DECLARED", l, dent, "declared band-aid: " + dent["retirement"]))
            continue
        rows.append(("FAIL", l, None, "guest-touching host launch, not master-gated, not allowlisted"))

    # open_forcing "site" entries (forcing that is not a host launch)
    site_rows = []
    by_rel = {s.rel: s for s in srcs}
    for ent in open_forcing:
        if ent.get("kind") != "site":
            continue
        path = os.path.join(root, ent["file"])
        s = by_rel.get(ent["file"])
        if s is None:
            if not os.path.exists(path):
                site_rows.append(("MISSING", ent, [], "file not present (generated? regenerate first)"))
                continue
            s = Src(path, root)
        hits = [m.start() for m in re.finditer(ent["anchor"], s.raw)]
        if not hits:
            site_rows.append(("FIXED", ent, [], "anchor gone"))
            continue
        open_hits = [h for h in hits if not gate_of(s, h, mfor(s.rel))]
        if open_hits:
            site_rows.append(("OPEN", ent, [s.line_of(h) for h in open_hits], ""))
        else:
            site_rows.append(("FIXED", ent, [s.line_of(h) for h in hits], "master-gated"))

    fails = [r for r in rows if r[0] == "FAIL"]
    open_sites = [r for r in site_rows if r[0] == "OPEN"]
    stale = [e for e in allow + declared if id(e) not in used_allow]

    if mode == "--json":
        print(json.dumps({
            "launches": [{"status": st, "file": l.src.rel, "line": l.line, "kind": l.kind,
                          "entry": l.entry, "note": note,
                          "prims": [{"prim": k, "line": ln, "code": c} for k, ln, c in l.prims]}
                         for st, l, _e, note in rows],
            "open_sites": [{"status": st, "file": e["file"], "lines": lns, "what": e.get("what")}
                           for st, e, lns, _n in site_rows],
        }, indent=2), file=out)
        return 1 if (fails or open_sites or bad_cls or bad_decl) else 0

    def p(*a):
        print(*a, file=out)

    p("=" * 78)
    p("HOST-LAUNCH AUDIT (scripts/audit_host_threads.py) -- threads / timers / VEH / atexit")
    p("that write guest memory, call guest code or signal guest-visible objects")
    p("=" * 78)
    cnt = {}
    for r in rows:
        cnt[r[0]] = cnt.get(r[0], 0) + 1
    p(f"  launch sites: {len(rows)}   FAIL {cnt.get('FAIL', 0)}   DECLARED {cnt.get('DECLARED', 0)}   "
      f"ALLOW {cnt.get('ALLOW', 0)}   MASTER {cnt.get('MASTER', 0)}   INERT {cnt.get('INERT', 0)}")
    p(f"  master-gate predicates recognized: {', '.join(sorted(gmasters))} (+ file-local bool vars)")
    p("")

    def fmt_prims(l, n=4):
        for k, ln, c in l.prims[:n]:
            p(f"        <{k}> :{ln}  {c}")
        if len(l.prims) > n:
            p(f"        ... +{len(l.prims) - n} more")

    def verdict_for(l):
        for e in open_forcing:
            if e.get("kind", "launch") == "launch" and entry_matches(e, l):
                return e
        return None

    if mode == "--all":
        for st in ("MASTER", "ALLOW", "INERT"):
            sel = [r for r in rows if r[0] == st]
            p(f"--- {st} ({len(sel)}) ---")
            for _st, l, e, note in sel:
                tag = f"{e['class']}: {e['reason'][:90]}" if e else note
                p(f"  {l.src.rel}:{l.line}  {l.kind} -> {l.entry}   [{tag}]")
            p("")

    decl_rows = [r for r in rows if r[0] == "DECLARED"]
    if decl_rows:
        p(f"--- DECLARED BAND-AIDS: live forcing launches with a retirement condition ({len(decl_rows)}) ---")
        for _st, l, e, _note in decl_rows:
            p(f"  DECLARED [{e['id']}]  {l.src.rel}:{l.line}  {l.kind} -> {l.entry}()")
            p(f"      why kept: {e['reason']}")
            p(f"      retire:   {e['retirement']}   ({e['bandaids']})")
        p("")
    if fails:
        p(f"--- FAIL: guest-touching host launches with no master gate ({len(fails)}) ---")
        for _st, l, e, note in fails:
            v = verdict_for(l)
            label = f"FORCING [{v['id']}]" if v else "UNREVIEWED"
            p(f"  {label}  {l.src.rel}:{l.line}  {l.kind} -> {l.entry}()")
            p(f"      why: {note}")
            if v:
                p(f"      what: {v['what']}")
                p(f"      fix:  {v['recommendation']}")
            fmt_prims(l)
        p("")
    if site_rows:
        p(f"--- OPEN FORCING (non-launch sites, docs/HOST_THREADS.json open_forcing) ---")
        for st, e, lns, note in site_rows:
            where = f"{e['file']}:{','.join(map(str, lns))}" if lns else e["file"]
            p(f"  {st:<7} [{e['id']}] {where}" + (f"  ({note})" if note else ""))
            if st == "OPEN":
                p(f"      what: {e['what']}")
                p(f"      fix:  {e['recommendation']}")
        p("")
    resolved = [e for e in open_forcing if e.get("kind", "launch") == "launch"
                and not any(r[0] == "FAIL" and entry_matches(e, r[1]) for r in rows)]
    if resolved:
        p("--- open_forcing launch entries no longer failing (fixed? drop the entry) ---")
        for e in resolved:
            p(f"  [{e['id']}] {e['file']}  {e.get('entry') or e.get('anchor')}")
        p("")
    if stale:
        p("--- allow entries matching no guest-touching launch (stale? review) ---")
        for e in stale:
            p(f"  {e['file']}  entry={e.get('entry')} anchor={e.get('anchor')!r}")
        p("")
    if bad_decl:
        p("--- declared entries missing reason / retirement / bandaids (IGNORED, audit fails) ---")
        for e in bad_decl:
            p(f"  {e.get('file')}  {e.get('entry') or e.get('anchor')}  id={e.get('id')}")
        p("")
    if bad_cls:
        p("--- allow entries with a class other than platform/diagnostic or no reason (IGNORED) ---")
        for e in bad_cls:
            p(f"  {e.get('file')}  {e.get('entry') or e.get('anchor')}  class={e.get('class')}")
        p("")

    if fails or open_sites or bad_cls or bad_decl:
        p(f"RESULT: HOST-LAUNCH AUDIT FAILS -- {len(fails)} launch site(s) and "
          f"{len(open_sites)} open forcing site(s). Master-gate them (launch inside "
          f"if (forcingAllowed())), delete them, or -- only if platform/observe -- "
          f"register them in {REGISTRY.replace(os.sep, '/')} with a class and reason.")
        return 1
    p("RESULT: every guest-touching host launch is master-gated or registered "
      f"(platform/diagnostic), {len(decl_rows)} declared band-aid(s) listed above, "
      "and no open forcing site remains.")
    return 0


if __name__ == "__main__":
    sys.exit(run(ROOT, sys.argv[1] if len(sys.argv) > 1 else "--summary"))
