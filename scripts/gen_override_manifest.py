#!/usr/bin/env python3
"""iter-389: kernel override registry generator.

Scans kernel/*.cpp for strong-def overrides of recompiled guest functions
(`void sub_XXXXXXXX(PPCContext&...)` and `PPC_FUNC(sub_XXXXXXXX)`) and
maintains docs/OVERRIDES.json — the reviewable registry the forcing audit
enforces. Every override must be classified:

  observe        pure logging; calls __imp__ exactly once, unconditionally,
                 mutates nothing the guest sees (auto-classifiable)
  platform-fix   implements what the console provided (charter-allowed);
                 needs evidence + an A/B note
  diagnostic     behavior change gated by an SR_* env, default OFF
  band-aid       behavior change that is NOT console parity; must name its
                 retirement condition (ledger: docs/BANDAIDS.md)
  devtool        iter-463 phase 2 (docs/DEVTOOLS_DESIGN.md 2.2): a dev-tool hook (the
                 level-skip drain point). Assigned MECHANICALLY and only to the exact
                 shape `if (!devToolHookNeeded_sub_X()) { __imp__sub_X(ctx, base); return; }`
                 at the top of the body (gate off = an exact pass-through), with no guest
                 register write anywhere in the body; any other shape that names a
                 devToolHookNeeded_* predicate is demoted to needs-review.
  needs-review   auto-detected mutation, not yet human/LLM classified
  native         iter-464 (docs/NATIVE_ROADMAP.md §9 CHANGE): a native PC replacement of a
                 console-hardware-facing XDK/XAudio entry (the shipping path from N1 on)
  oracle         iter-464: part of the PM4/XMA/hardware EMULATION kept as the test oracle
                 (doorbell -> emulated CP, synthetic ISR bits, register front ends, emulation-
                 path hang fixes, roadmap E4). Never counted as native progress; removed from
                 the shipping path at N7
  dead(static)   iter-464: an override of a function proven dead (static + witness); kept only
                 as a tripwire
  port-feature   iter-464 (docs/MODS.md "Port features"): a player-facing PC feature that changes
                 what the game does on purpose (mouse-look, air moves or render-loop rate).
                 Its own SR_* gate declares the feature; input features are inert without input,
                 and render-rate changes are OFF in automated rolls unless explicitly selected.
                 Gate-off delegates to the game's body; never counted as console parity.

Manual fields (`class`, `evidence`, `retire`, `default_exe`) persist across regenerations;
mechanical fields (`file`, `gates`, `auto`) are refreshed each run.

Multiple definitions retain their own conditions/gates/classification in
`variants`. Top-level `gates_scope=all-source-variants` is an inventory union,
never proof that a gate protects another variant. The forcing audit classifies
each write from its source-site control flow; its registry reader uses manual
classes, not this gate union. Consumers needing execution semantics must select
and validate a build configuration rather than infer one from this inventory.

R1 (docs/SPLICE_CONVERSION.md): tools/regen/superman_regen.toml [[midasm_hook]] entries are
registered too, keyed "midasm:0x<ADDR>". Every hook and every R1 port (an override in
kernel/r1_*.cpp, or one that calls an sr::r1:: helper) carries `default_exe`, which says where
its forced behaviour acts on the default exe (DEFAULT_EXE below); scripts/audit_forcing.py
counts the `live` and `conditional` band-aids in the forcing number and fails on a hook or
port without it.
"""
import json
import functools
import bisect
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KERNEL = ROOT / "kernel"
MANIFEST = ROOT / "docs" / "OVERRIDES.json"
HAND_OVERRIDE_SOURCES = ["tools/regen/hand/ppc_missing_recomp_stubs.cpp"]   # relative to ROOT

# `default_exe` values (manual, R1 hooks and ports):
#   live         the forced behaviour acts in normal play on the default exe (a fixed cap that
#                recurs, a skipped wait, a stub that always runs, a wall-clock abort) - forcing
#   conditional  a guard / cap / fake result that acts only when its condition holds (a bad
#                pointer, a runaway loop, a stack-depth limit) - forcing, counted
#   support      state another hook of the same port reads (a counter reset, a clock start);
#                no effect of its own
#   observe      a log / metric marker; no guest-visible effect
#   platform     a platform fix (what the console provided)
#   gated        acts only behind sr::kernel::forcingAllowed() (SR_ALLOW_FORCING)
DEFAULT_EXE = ("live", "conditional", "support", "observe", "platform", "gated")

DEF_RE = re.compile(
    r"^(?:void\s+(sub_[0-9A-Fa-f]{8})\s*\(\s*PPCContext|PPC_FUNC\((sub_[0-9A-Fa-f]{8})\))",
    re.M,
)
GATE_RE = re.compile(r'getenv\("(SR_[A-Z0-9_]+)"\)')
# iter-415: gates are very often reached through a one-line predicate helper
# (`static bool fooEnabled() { ... getenv("SR_FOO") ... }`) rather than a getenv
# in the override body itself. Scanning only the body under-reported those gates
# as [] - which matters, because audit_forcing.py uses `gates` to decide whether
# a registry-classified band-aid is active by default. Follow ONE level of
# zero-arg helper calls defined in the same file.
HELPER_DEF_RE = re.compile(
    r"^\s*(?:static\s+|inline\s+)*bool\s+(\w+)\s*\(\s*(?:void)?\s*\)\s*\{", re.M)
HELPER_CALL_RE = re.compile(r"\b(\w+)\s*\(\s*\)")


def gates_in(body, helpers):
    """Gates named directly in `body`, plus those in zero-arg helpers it calls."""
    found = set(GATE_RE.findall(body))
    for name in set(HELPER_CALL_RE.findall(body)):
        hb = helpers.get(name)
        if hb:
            found |= set(GATE_RE.findall(hb))
    return sorted(found)


# \u26d4 iter-457 r6, DIRECTOR DEFECT 2. An override can be gated WITHOUT naming
# its gate: `sub_82100310` reads a file-scope flag and the getenv sits in the
# SETTER that writes it, so the registry recorded `gates: []` while the prose
# claimed structural gating. `gates` is regenerated mechanically every run and
# audit_forcing.py reads it to decide whether a classified band-aid is live by
# default - so an empty list there is a false statement about the code, and prose
# is no substitute.
#
# The rule, and the reason it is sound: a flag's value can only come from the
# code that WRITES it. If every writer sits behind a getenv gate, that gate
# controls the flag, and therefore controls the reader.
# ⛔ INDENTATION IS NOT THE TEST - _file_scope_flags's function-span
# exclusion is. r6 required column 0 for the non-static form to keep locals
# out, and that silently missed a valid namespace-scope declaration:
#
#     namespace hidden {
#       bool s_flag = false;
#       void writer() { s_flag = std::getenv("SR_REAL") != nullptr; }
#     }
#
# A wrapper reading hidden::s_flag reported NO gate. Both arms now allow
# leading whitespace; locals are excluded by being inside a function body,
# which is the property that actually distinguishes them.
_FLAG_TYPE = r"(?:std::atomic\s*<\s*bool\s*>|std::atomic_bool|bool)"
STATIC_FLAG_RE = re.compile(
    r"^\s*static\s+(?:const\s+)?" + _FLAG_TYPE + r"\s+(\w+)\s*[{=;]"
    r"|^\s*" + _FLAG_TYPE + r"\s+(\w+)\s*[{=;]",
    re.M)
# A definition whose body we can extract: `T name(args) {` starting a line.
# \u26d4 Leading whitespace allowed: a definition inside `namespace hidden {`
# is indented, and requiring column 0 made _enclosing_function blind to it -
# which is half of why the director's namespace-scope counterexample reported no
# gate. The keyword guard is what stops `if (cond) {` being read as a function
# named `if`.
FUNC_DEF_RE = re.compile(
    r"^\s*(?!(?:if|for|while|switch|catch|return|else|do)\b)"
    r"[A-Za-z_][\w:<>,&*\s]*?\b(\w+)\s*\([^;{}]*\)\s*(?:const\s*)?\{", re.M)


def _writes_flag(text: str, flag: str):
    """Offsets in `text` where `flag` is assigned or .store()d."""
    pat = re.compile(re.escape(flag) + r"\s*\.\s*store\s*\("
                     r"|\b" + re.escape(flag) + r"\s*=(?!=)")
    return [m.start() for m in pat.finditer(text)]


@functools.lru_cache(maxsize=64)
def _function_spans(text: str):
    """[(body_start, body_end, name, body)] for every definition in `text`.

    Capped at the next definition, because `extract_body` counts raw braces
    and does not skip comments or literals: one unbalanced brace in a comment
    makes a body swallow the rest of the file.
    """
    ms = list(FUNC_DEF_RE.finditer(text))
    spans = []
    for i, m in enumerate(ms):
        b0 = text.find("{", m.end() - 1)
        if b0 < 0:
            continue
        body = extract_body(text, m.end() - 1)
        limit = ms[i + 1].start() if i + 1 < len(ms) else len(text)
        b1 = min(b0 + len(body), limit)
        spans.append((b0, b1, m.group(1), text[b0:b1]))
    return tuple(spans)


def _enclosing_function(text: str, offset: int):
    """The SMALLEST definition span containing `offset`, or None."""
    best = None
    for b0, b1, name, body in _function_spans(text):
        if b0 <= offset < b1 and (best is None or (b1 - b0) < (best[1] - best[0])):
            best = (b0, b1, name, body)
    return None if best is None else (best[2], best[3])


# A class/struct/union BODY is not file scope either. Relaxing the indentation
# rule (director defect 2) let `struct Foo { bool live; };` register `live` as a
# file-scope flag, and seven overrides that merely mention a member of that name
# collected SR_S6_WINFIX. Members are excluded by the same span test as locals.
TYPE_DEF_RE = re.compile(
    r"^\s*(?:typedef\s+)?(?:struct|class|union|enum)\b[^;{}]*\{", re.M)


@functools.lru_cache(maxsize=64)
def _type_spans(text: str):
    """[(body_start, body_end)] for every class/struct/union/enum body."""
    ms = list(TYPE_DEF_RE.finditer(text))
    spans = []
    for i, m in enumerate(ms):
        b0 = text.find("{", m.end() - 1)
        if b0 < 0:
            continue
        body = extract_body(text, m.end() - 1)
        limit = ms[i + 1].start() if i + 1 < len(ms) else len(text)
        spans.append((b0, min(b0 + len(body), limit)))
    return tuple(spans)


def _file_scope_flags(text: str):
    """Flag names declared OUTSIDE every function body.

    Indentation is NOT the test - being outside every function body is.
    r6 required column 0 for the non-static form, which missed a valid
    `namespace hidden { bool s_flag = false; }` (director defect 2). The
    function-span exclusion is what keeps LOCALS out, and it does that job
    whatever the indentation.
    """
    spans = _function_spans(text)
    out = set()
    for m in STATIC_FLAG_RE.finditer(text):
        name = next((g2 for g2 in m.groups() if g2), None)
        if not name:
            continue
        if any(b0 <= m.start() < b1 for b0, b1, _n, _b in spans):
            continue                      # function-local, not a file-scope flag
        if any(b0 <= m.start() < b1 for b0, b1 in _type_spans(text)):
            continue                      # a class/struct MEMBER, not a flag
        out.add(name)
    return out


# \u26d4 THE CANONICAL GATED-SETTER SHAPE, and nothing else.
#
# r6 attributed every gate found anywhere in a writer function. The director
# refuted that in one line:
#
#     void writer() {
#       if (std::getenv("SR_FAKE")) note();   // unrelated
#       s_flag.store(true);                   // UNCONDITIONAL write
#     }
#
# A reader of s_flag then reported ["SR_FAKE"] - a fabricated gate, which is
# the dangerous direction, because audit_forcing.py reads `gates` to decide
# whether a classified band-aid runs by default.
#
# So this is deliberately NOT a control-flow engine. It recognizes ONE shape
# and refuses everything else:
#
#   1. the gate is READ INTO A NAMED LATCH   `... inSuite = ... getenv("G") ...`
#   2. that latch GUARDS the write           `if (!inSuite) ...` before the store
#
# Both must hold, in the same function, with the guard textually before the
# write. Anything else contributes NO gate. Fail-closed means an unrecognized
# setter under-reports, which audit_forcing.py treats as ungated - the safe
# direction.
LATCH_DECL_RE = re.compile(
    r"\b(?:const\s+)?(?:bool|auto)\s+(\w+)\s*=[^;]*?"
    r'getenv\s*\(\s*"(SR_[A-Z0-9_]+)"',
    re.S)


def _gated_latches(fbody: str):
    """[(latch_name, gate)] for `bool NAME = ... getenv("SR_X") ...` decls."""
    return [(m.group(1), m.group(2)) for m in LATCH_DECL_RE.finditer(fbody)]


def _write_statement(fbody: str, write_at: int) -> str:
    """The single statement containing the write at `write_at`."""
    lo = max(fbody.rfind(";", 0, write_at), fbody.rfind("{", 0, write_at),
             fbody.rfind("}", 0, write_at))
    hi = fbody.find(";", write_at)
    return fbody[lo + 1:(hi + 1) if hi >= 0 else len(fbody)]


def _latch_guards_write(fbody: str, latch: str, write_at: int) -> bool:
    """True when an `if (...latch...)` appears BEFORE the write in `fbody`."""
    head = strip_comments_and_strings(fbody[:write_at])
    return bool(re.search(r"\bif\s*\([^)]*\b"
                          + re.escape(latch) + r"\b", head))


def flag_gates(body: str, text: str, helpers) -> set:
    """Gates that reach `body` through a FILE-SCOPE flag it READS.

    Only the canonical gated-setter shape counts: a getenv latched into a
    named local, and that latch guarding the write. See LATCH_DECL_RE.
    """
    out = set()
    flags = [f for f in _file_scope_flags(text)
             if re.search(r"\b" + re.escape(f) + r"\b", body)]
    if not flags:
        return out
    body_at = text.find(body) if body else -1
    for flag in flags:
        for off in _writes_flag(text, flag):
            if body_at >= 0 and body_at <= off < body_at + len(body):
                continue          # the override writing its own flag is not a gate
            enc = _enclosing_function(text, off)
            if not enc:
                continue
            fname, fbody = enc
            fstart = text.find(fbody)
            if fstart < 0:
                continue
            rel = off - fstart
            # SHAPE A - the written VALUE is the gate:
            #     s_flag = std::getenv("SR_REAL") != nullptr;
            # Here the flag does not merely sit behind the gate, it IS the gate,
            # which is the most direct form of "this gate controls this flag".
            # Scoped to the write STATEMENT, so an unrelated getenv elsewhere in
            # the function contributes nothing.
            out |= set(GATE_RE.findall(_write_statement(fbody, rel)))
            # SHAPE B - a latched getenv guards the write:
            #     static const bool inSuite = [] { ... getenv("G") ... }();
            #     if (!inSuite) { flag.store(false); return false; }
            for latch, gate in _gated_latches(fbody):
                if _latch_guards_write(fbody, latch, rel):
                    out.add(gate)
    return out

# Signals that the wrapper does more than log-and-delegate.
MUTATION_RE = re.compile(
    r"writeGuest|writeBE|std::memset\(\s*host|"
    # `=` must not be `==`: `ctx.r3.u32 == 0` is a READ, and counting it as a
    # mutation flagged a pile of pure logging hooks.
    r"ctx\.r\d+\.\w+\s*=(?!=)|ctx\.f\d+\s*=(?!=)|"
    r"ctx\.lr\s*=(?!=)|guestVAToHost\([^)]*\)\s*\)\s*=(?!=)|host\[\d"
    # `return;` was in this list too. In a void PPC hook that is ordinary
    # control flow (log, then early-out), not a guest-state change — it said
    # nothing about whether the override mutates anything.
)


def extract_body(text: str, start: int) -> str:
    brace = text.find("{", start)
    if brace < 0:
        return ""
    depth, i = 0, brace
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[brace : i + 1]
        i += 1
    return text[brace:]


# A forward declaration is not a call. For literal definitions the `extern "C"`
# line sits outside the extracted body so this never mattered, but a macro body
# carries its own declaration — which made every macro override count imp=2 and
# land in needs-review, i.e. the blind spot closed in iter-402 was immediately
# replaced by 110 misclassified entries.
IMP_DECL_RE = re.compile(r'extern\s+"C"\s+void\s+__imp__\w+\s*\([^;{]*\)\s*;')


def why_flagged(sym: str, body: str) -> str:
    """Auto-derived mechanism note for anything that is not plain observe.

    A bare list of 21 symbols tells a reviewer nothing; what they need is the
    delegate count and the mutating statement, with whether it runs before or
    after the delegate (before + early return = replacement path, after =
    return-value substitution). Always recomputed, never hand-edited.
    """
    named, ok = devtool_shape(sym, body)
    if named:
        return ("devtool hook: exact pass-through when devToolHookNeeded_%s() is false" % sym if ok else
                "devtool hook NOT in the pass-through shape (if (!devToolHookNeeded_%s()) { __imp__%s(ctx, "
                "base); return; } first, no register write)" % (sym, sym))
    calls_only = IMP_DECL_RE.sub("", body)
    imp_at = [m.start() for m in re.finditer(rf"__imp__{sym}\s*\(", calls_only)]
    parts = [f"imp_calls={len(imp_at)}"]
    if not imp_at:
        parts.append("NEVER delegates (full replacement)")
    first = imp_at[0] if imp_at else None
    seen = []
    for m in MUTATION_RE.finditer(calls_only):
        line = calls_only[calls_only.rfind("\n", 0, m.start()) + 1 :]
        line = line.split("\n")[0].strip()[:90]
        when = "after" if (first is not None and m.start() > first) else "before"
        tag = f"{when}: {line}"
        if tag not in seen:
            seen.append(tag)
    scope = delegate_scope(sym, body)
    if scope == "conditional":
        # iter-457 r5: a reviewer must see WHY this is not plain observe.
        parts.append("delegation is CONDITIONAL - the guest body can be skipped")
    elif scope == "indeterminate":
        # \u26a0 Say what is true. The delegate sits in a LAMBDA, so whether it
        # runs is the callee's decision and is not visible here. Calling that
        # "conditional" would assert more than we checked.
        parts.append("delegation is inside a LAMBDA - whether it runs is the "
                     "callee's decision, not visible here")
    parts.extend(seen[:3])
    return " | ".join(parts)


# A raw literal opens as R"delim( and closes at )delim" - up to 16 delimiter
# characters, none of them whitespace, parentheses or backslash. Encoding
# prefixes (u8R, LR, uR, UR) are permitted before the R.
RAW_OPEN_RE = re.compile(r"(?:u8|L|u|U)?R" + chr(34) + r"([^()\\ ]{0,16})" + r"\(")


def _raw_literal_end(text: str, i: int):
    """Index just past the raw literal starting at `i`, or None."""
    m = RAW_OPEN_RE.match(text, i)
    if not m:
        return None
    close = ")" + m.group(1) + chr(34)
    j = text.find(close, m.end())
    return len(text) if j < 0 else j + len(close)


def is_definition(text: str, end: int) -> bool:
    """True when the match at `end` opens a BODY rather than ending in `;`.

    ⛔ THE DEFECT THE DIRECTOR CAUGHT (iter-457 r5). `DEF_RE` matches
    `void sub_82100310(PPCContext` — which is equally the start of a strong
    definition AND of a forward DECLARATION. kernel/tex_unlock_test.cpp declares
    that symbol so a fixture can call the real override, and because scan()
    walks files in sorted order and later writes win, `tex_unlock_test.cpp`
    overwrote the true owner `hle_api_observe.cpp`. docs/OVERRIDES.json then
    named the TEST as the owner of a production override, with no gates, and
    audit_forcing.py passed on that — a FALSE NEGATIVE in the one registry whose
    entire purpose is that new behavior cannot land invisibly.

    Scans forward past the parameter list for the first `{` or `;` at paren
    depth 0. `;` first means a declaration, and it is skipped.
    """
    depth = 0
    i = end
    n = len(text)
    while i < n:
        c = text[i]
        # ⛔ COMMENTS AND STRINGS ARE SKIPPED. The first version of this scanner
        # dropped four REAL overrides (sub_826C2790 and friends) because
        # `PPC_FUNC(sub_826C2790)` carries a trailing `// ... len2,3: t[c1]; ...`
        # comment, and the SEMICOLONS inside it read as a declaration
        # terminator. A scanner that cannot tell code from commentary silently
        # un-registers overrides, which is the same class of failure as the
        # defect it was written to fix.
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find(chr(10), i)
            i = n if j < 0 else j + 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        # ⛔ STRING AND CHARACTER LITERALS ARE SKIPPED, and r5 was WRONG to
        # delete this. I argued the skip was unreachable because every string
        # between the match and the body must sit inside parentheses, so the
        # paren-depth guard would reach it first. The director REFUTED that
        # with a counterexample it compiled: a C++20 trailing requires-clause
        # puts a literal at paren depth ZERO.
        #
        #   template <class T>
        #   void sub_82ABCDE0(PPCContext& ctx, uint8_t* b)
        #       requires ";"_ok
        #   { ... }
        #
        # The `;` inside that literal is a real definition read as a
        # declaration, which drops the override from the registry - the exact
        # failure the comment-skipping above exists to prevent. "I could not
        # kill the mutant" was evidence my FIXTURES were too narrow, not that
        # the code was dead; the fixture that kills it is now in the suite.
        # \u26d4 RAW literals first, because they are NOT escape-processed and a
        # quote inside their payload is ordinary text. The director's second
        # counterexample compiles and was misread by the ordinary-literal skip:
        #
        #     requires R"tag(";{)tag"_ok
        #
        # The `"` inside the payload ended the skip early, the `;` after it
        # terminated the scan, and a real definition read as a declaration -
        # the same silent override-drop, in a new valid form.
        raw = _raw_literal_end(text, i)
        if raw is not None:
            i = raw
            continue
        if c in ("'", chr(34)):
            q = c
            i += 1
            while i < n and text[i] != q:
                i += 2 if text[i] == chr(92) else 1
            i += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif depth <= 0:
            if c == "{":
                return True
            if c == ";":
                return False
        i += 1
    return False


# iter-457 r5: does the delegate call sit under a CONDITION?
# `if (!bypass) __imp__sub_XXXX(ctx, base);` still contains exactly one call and
# mutates nothing, so the old rule called it `observe` — but "observe" asserts
# UNCONDITIONAL delegation, and a wrapper that can skip the guest body is a
# materially different contract. Detected two ways, either sufficient: the call
# is nested inside a block, or the statement it belongs to is guarded by an
# `if`/`else`/ternary.
# The text between the previous statement boundary and the delegate call IS
# the guard, if there is one. A keyword anywhere in that slice is therefore
# sufficient - the earlier form required the keyword to sit at the very end,
# so `if (!bypass) __imp__...` (the exact shape under test) never matched.
COND_KEYWORD_RE = re.compile(r"\b(if|else|while|for)\b")


def strip_comments_and_strings(t: str) -> str:
    """Blank out //, /* */ and string/char literals, preserving length.

    Brace and semicolon counting has to read CODE. An `if` in a comment, or a
    brace in a log format string, otherwise changes the verdict - which is the
    same class of error as the semicolon-in-a-comment that dropped four
    band-aids from the registry.
    """
    out = list(t)
    i, n = 0, len(t)
    while i < n:
        c = t[i]
        if c == "/" and i + 1 < n and t[i + 1] == "/":
            j = t.find(chr(10), i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
            continue
        if c == "/" and i + 1 < n and t[i + 1] == "*":
            j = t.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                out[k] = " "
            i = j
            continue
        if c in ("'", chr(34)):
            q = c
            j = i + 1
            while j < n and t[j] != q:
                j += 2 if t[j] == chr(92) else 1
            j = min(j + 1, n)
            for k in range(i, j):
                out[k] = " "
            i = j
            continue
        i += 1
    return "".join(out)


def _governor(head: str, brace_at: int) -> str:
    """The text that GOVERNS the block opening at `brace_at`.

    Everything back to the previous statement boundary. `} catch (...) {` gives
    " catch (...) " because the `}` is a boundary, which is what lets a catch
    block be told apart from the try block it follows.
    """
    cut = max(head.rfind(";", 0, brace_at),
              head.rfind("{", 0, brace_at),
              head.rfind("}", 0, brace_at))
    return head[cut + 1:brace_at]


def _is_conditional_governor(gov: str) -> bool:
    if COND_KEYWORD_RE.search(gov):            # if / else / while / for
        return True
    # \u26a0 `do` is NOT here, deliberately. `do { __imp__X(...); } while (c);`
    # runs its body at least once, so the delegate is unconditional - the
    # director caught this as a false positive. A real condition NESTED inside
    # the do-body is still found, because that block has its own governor.
    if re.search(r"\b(switch|catch)\b", gov):
        return True
    return "?" in gov or "&&" in gov or "||" in gov


def _is_lambda_governor(gov: str) -> bool:
    """`[](Args) {`, `[&] {`, `[=]() -> T {` - a lambda BODY, not a scope."""
    return bool(re.search(r"\][^;{}]*$", gov))


def delegate_scope(sym: str, body: str) -> str:
    """"" | "conditional" | "indeterminate" - what governs the delegate call.

    \u26d4 iter-457 r5b, A DEFECT THIS FUNCTION SHIPPED WITH. The first version
    tested raw brace DEPTH: any call nested below the function's own brace was
    "conditional". That is false for `try { __imp__X(...); }`, where the guest
    body always runs - and it demoted a real override (sub_8235F488) on exactly
    that shape. Depth is not a condition; the KEYWORD GOVERNING the block is.

    So walk the blocks still open at the call and ask what opened each:
      if / else / while / for / switch / catch / do, or ?: / && / ||
                                    -> CONDITIONAL, the body can be skipped.
      a lambda introducer           -> INDETERMINATE. Whether the lambda runs is
                                       decided by the callee it is handed to,
                                       which is not visible from here. Saying
                                       "the guest body can be skipped" would
                                       assert more than the scanner knows;
                                       saying "observe" would assert less.
      try / a plain scope / nothing -> unconditional.
    """
    m = re.search(rf"__imp__{sym}\s*\(", body)
    if not m:
        return ""
    head = strip_comments_and_strings(body[:m.start()])

    # (a) the still-open blocks, skipping the function's OWN outer brace -
    # `extract_body` includes it, so an unconditional top-level call already
    # sits inside one block.
    stack = []
    for i, c in enumerate(head):
        if c == "{":
            stack.append(i)
        elif c == "}" and stack:
            stack.pop()
    verdict = ""
    for brace_at in stack[1:]:
        gov = _governor(head, brace_at)
        if _is_conditional_governor(gov):
            return "conditional"
        if _is_lambda_governor(gov):
            verdict = "indeterminate"

    # (b) the statement itself is guarded, with no braces at all:
    # `if (!bypass) __imp__X(ctx, base);`
    cut = max(head.rfind(";"), head.rfind("{"), head.rfind("}"))
    stmt = head[cut + 1:]
    # The SAME governor test as the block walk. Using a narrower test here
    # let a short-circuit guard (`ready() && __imp__X(...)`) read as
    # unconditional while the identical guard on a braced block did not.
    # \u26a0 The ternary case lives inside _is_conditional_governor with the
    # rest. It used to be a separate arm here, which the mutation harness
    # correctly reported as UNKILLABLE once the governor test was unified -
    # deleting subsumed code changed no outcome. One test, one mutant, one
    # place to be wrong.
    if _is_conditional_governor(stmt):
        return "conditional"
    return verdict


def delegate_conditional(sym: str, body: str) -> bool:
    """Anything the scanner cannot certify as unconditional delegation."""
    return delegate_scope(sym, body) != ""


DEVTOOL_PRED_RE = re.compile(r"\bdevToolHookNeeded_\w+\s*\(")


def devtool_shape(sym: str, body: str):
    """iter-463 phase 2: (names a dev-tool predicate, is the exact pass-through shape).
    The shape: the body OPENS with `if (!devToolHookNeeded_<sym>()) { __imp__<sym>(ctx, base);
    return; }` (comments ignored), the predicate is the one named after this function, and
    no guest register is written anywhere in the body (MUTATION_RE)."""
    clean = strip_comments_and_strings(IMP_DECL_RE.sub("", body))
    if not DEVTOOL_PRED_RE.search(clean):
        return False, False
    head = re.match(
        r"\{\s*if\s*\(\s*!\s*(?:sr\s*::\s*kernel\s*::\s*)?devToolHookNeeded_" + sym +
        r"\s*\(\s*\)\s*\)\s*\{\s*__imp__" + sym + r"\s*\(\s*ctx\s*,\s*base\s*\)\s*;\s*return\s*;\s*\}", clean)
    return True, bool(head) and not MUTATION_RE.search(clean)


def classify_auto(sym: str, body: str) -> str:
    # Same input as why_flagged, so the verdict and its stated reason can never
    # disagree — four entries were flagged with no reportable mutation because
    # these two read different text.
    named, ok = devtool_shape(sym, body)
    if named:
        return "devtool" if ok else "needs-review"
    calls_only = IMP_DECL_RE.sub("", body)
    imp_calls = len(re.findall(rf"__imp__{sym}\s*\(", calls_only))
    mutated = bool(MUTATION_RE.search(calls_only))
    if imp_calls == 1 and not mutated and not delegate_conditional(sym, body):
        return "observe"
    return "needs-review"


# iter-402: MACRO-GENERATED strong-defs were invisible to this scanner, so a
# whole family of overrides bypassed classification silently — 19 of them from
# SR_MPFLAG_HOOK alone. The registry's promise is "new behavior cannot land
# invisibly", which a text scan that only understands literal definitions
# cannot keep. Resolve function-like macros whose FIRST argument is a
# sub_XXXXXXXX symbol, and classify each invocation using the macro's own body
# with the parameter substituted (so `__imp__##sym(` reads as a real call).
MACRO_DEF_RE = re.compile(
    r"^[ \t]*#define[ \t]+([A-Z][A-Z0-9_]*)\(([^)]*)\)(.*?)(?<!\\)$",
    re.M | re.S,
)
MACRO_CALL_RE = re.compile(
    r"^[ \t]*([A-Z][A-Z0-9_]*)\(\s*(sub_[0-9A-Fa-f]{8})\s*[,)]", re.M
)


def collect_macros(text: str) -> dict:
    """name -> source definitions. Only macros that strong-def a guest fn."""
    out = {}
    for m in MACRO_DEF_RE.finditer(text):
        name, params, body = m.group(1), m.group(2), m.group(3)
        first = params.split(",")[0].strip()
        if not first:
            continue
        # Must actually define a PPC entry point, else it is just a helper.
        if "PPCContext" not in body and "PPC_FUNC" not in body:
            continue
        out.setdefault(name, []).append((first, body, m.start()))
    return out


def macro_body_for(sym: str, param: str, body: str) -> str:
    """Substitute the macro parameter with the real symbol, dropping ## pastes."""
    # Substitute the pastes FIRST and straight to the symbol: turning `##sym`
    # into `sym` would leave `__imp__sym`, where a \bsym\b word-boundary match
    # can never fire (underscore is a word character) — which silently made
    # every macro override look like it never calls __imp__.
    resolved = body.replace("##" + param, sym).replace(param + "##", sym)
    return re.sub(rf"\b{re.escape(param)}\b", sym, resolved)


def directive_text(text: str):
    """Return directive mask, comment-free text, and original character offsets.

    C++ line splicing precedes comment recognition, including continued //
    comments and split comment delimiters. Literals hide directive-looking
    text, but remain intact in the second result for condition provenance.
    This is lexical inventory only; macros and conditions are not evaluated.
    """
    chars, offsets = [], []
    start = 0
    for splice in re.finditer(r"\\\r?\n", text):
        chars.append(text[start:splice.start()])
        offsets.extend(range(start, splice.start()))
        start = splice.end()
    chars.append(text[start:])
    offsets.extend(range(start, len(text)))
    logical = "".join(chars)
    mask, expressions = list(logical), list(logical)
    i, n = 0, len(logical)
    while i < n:
        comment = False
        if logical.startswith("//", i):
            end = logical.find("\n", i)
            end = n if end < 0 else end
            comment = True
        elif logical.startswith("/*", i):
            end = logical.find("*/", i + 2)
            end = n if end < 0 else end + 2
            comment = True
        else:
            end = _raw_literal_end(logical, i)
            if end is None and logical[i] in ('"', "'"):
                quote, end = logical[i], i + 1
                while end < n:
                    if logical[end] == "\\":
                        end += 2
                    elif logical[end] == quote:
                        end += 1
                        break
                    else:
                        end += 1
                end = min(end, n)
            elif end is None and (logical[i].isdigit() or
                                  (logical[i] == "." and i + 1 < n and logical[i + 1].isdigit())):
                # Apostrophes inside preprocessing numbers are digit
                # separators, not character-literal openers (1'000, 0xA'B).
                i += 1
                while i < n and (logical[i].isalnum() or logical[i] in "_.'"):
                    i += 1
                continue
            elif end is None:
                i += 1
                continue
        for pos in range(i, end):
            if logical[pos] not in "\r\n":
                mask[pos] = " " if comment else "x"
                if comment:
                    expressions[pos] = " "
        i = end
    return "".join(mask), "".join(expressions), offsets


def conditional_locations(text: str):
    """Lexical branch provenance, not preprocessor evaluation or reachability.

    Each nested list is the #if/#elif/#else chain selecting one branch. Keep
    #if 0 and unknown conditions: inventory must not certify an active build.
    Comments and literals cannot introduce directives.
    """
    positions, contexts, stack = [0], [[]], []
    clean, expressions, offsets = directive_text(text)
    for match in re.finditer(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b([^\n]*)", clean, re.M):
        kind = match.group(1)
        expression = expressions[match.start(2):match.end(2)].strip()
        directive = "#" + kind + (" " + expression if expression else "")
        if kind in ("if", "ifdef", "ifndef"):
            stack.append([directive])
        elif not stack:
            raise ValueError("unmatched preprocessor branch at line " + str(text.count("\n", 0, offsets[match.start()]) + 1))
        elif kind == "endif":
            stack.pop()
        elif any(part == "#else" for part in stack[-1]):
            raise ValueError("preprocessor branch after #else")
        else:
            stack[-1].append(directive)
        positions.append(offsets[match.end() - 1] + 1)
        contexts.append([list(chain) for chain in stack])
    if stack:
        raise ValueError("unterminated preprocessor conditional")
    return positions, contexts


def record_variant(variants, sym, mechanical, text, offset, locations):
    positions, contexts = locations
    variants.setdefault(sym, []).append(dict(
        mechanical, line=text.count("\n", 0, offset) + 1,
        conditions=contexts[bisect.bisect_right(positions, offset) - 1]))


def retain_variants(found, variants):
    for sym, definitions in variants.items():
        if len(definitions) < 2:
            continue
        entry = found[sym]
        entry["variants"] = definitions
        # Compatibility field for consumers doing source inventory. It is NOT
        # a claim that these gates or bodies execute together in any build.
        entry["gates_scope"] = "all-source-variants"
        entry["gates"] = sorted({gate for definition in definitions for gate in definition["gates"]})
        entry["auto"] = "needs-review"
        entry["why"] = "multiple source variants; active configuration not evaluated: " + "; ".join(
            f'{definition["file"]}:{definition["line"]} [{definition["auto"]}] {definition["why"]}'
            for definition in definitions)


def scan():
    found = {}
    variants = {}
    # iter-395: guest/*.cpp carries strong-defs too (ppc_import_glue.cpp hid a
    # FULL malloc substitution + an out-of-bounds bug for dozens of sessions —
    # the registry never saw it because only kernel/ was scanned). Scan both.
    scan_files = list(KERNEL.glob("*.cpp")) + list((ROOT / "guest").glob("*.cpp"))
    # iter-463 round 4: the hand-written guest stubs (compiled as
    # guest/ppc/ppc_missing_recomp_stubs.cpp, committed here) are strong defs
    # too - several REPLACE a recompiled weak body (sub_827A0000, sub_8246D5C8,
    # sub_8246D6C8, sub_827AC440) and several fabricate a return value at a
    # mid-function address. They must be classified like any other override.
    scan_files += [ROOT / p for p in HAND_OVERRIDE_SOURCES if (ROOT / p).is_file()]
    for f in sorted(scan_files):
        text = f.read_text(encoding="utf-8", errors="replace")
        locations = conditional_locations(text)
        # iter-415: zero-arg bool predicates in this file, so a gate reached via
        # `if (!fooEnabled()) ...` is attributed to the override that calls it.
        helpers = {}
        for hm in HELPER_DEF_RE.finditer(text):
            helpers[hm.group(1)] = extract_body(text, hm.end())
        for m in DEF_RE.finditer(text):
            sym = m.group(1) or m.group(2)
            # A forward declaration is not an override. See is_definition.
            if not is_definition(text, m.end()):
                continue
            body = extract_body(text, m.end())
            found[sym] = {
                "file": f.name,
                "gates": sorted(set(gates_in(body, helpers))
                                | flag_gates(body, text, helpers)),
                "auto": classify_auto(sym, body),
                "why": why_flagged(sym, body),
            }
            record_variant(variants, sym, found[sym], text, m.start(), locations)
        # Macro-generated strong-defs (see collect_macros).
        macros = collect_macros(text)
        for m in MACRO_CALL_RE.finditer(text):
            macro, sym = m.group(1), m.group(2)
            if macro not in macros:
                continue
            for param, mbody, definition_offset in macros[macro]:
                body = macro_body_for(sym, param, mbody)
                positions, contexts = locations
                found[sym] = {
                    "file": f.name,
                    "gates": sorted(set(gates_in(body, helpers))
                                    | flag_gates(body, text, helpers)),
                    "auto": classify_auto(sym, body),
                    "why": why_flagged(sym, body),
                    "via_macro": macro,
                    "macro_line": text.count("\n", 0, definition_offset) + 1,
                    "macro_conditions": contexts[bisect.bisect_right(positions, definition_offset) - 1],
                }
                record_variant(variants, sym, found[sym], text, m.start(), locations)
    retain_variants(found, variants)
    found.update(scan_midasm())
    return found


# R1 (docs/SPLICE_CONVERSION.md): tools/regen/superman_regen.toml [[midasm_hook]] entries change
# generated code from the inside, so they are registered like strong-def overrides, keyed
# "midasm:0x<ADDR>". `auto` is "observe" for a void hook with no jump / return that writes no
# register or guest memory, else "midasm" (a mutating hook: it must carry a manual class).
MIDASM_TOML = ROOT / "tools" / "regen" / "superman_regen.toml"
_HOOK_WRITE_RE = re.compile(r"\b(?:r\d+|f\d+|v\d+|ctr|cr\d|xer|fpscr|reserved)\.\w+\s*=(?!=)|"
                            r"PPC_STORE_|writeGuest|PPC_MM_STORE")


def scan_midasm():
    import tomllib
    out = {}
    if not MIDASM_TOML.is_file():
        return out
    hooks = tomllib.loads(MIDASM_TOML.read_text(encoding="utf-8")).get("midasm_hook", [])
    defs = {}
    for f in sorted(KERNEL.glob("*.cpp")):
        text = f.read_text(encoding="utf-8", errors="replace")
        for h in hooks:
            name = h.get("name", "")
            if name in defs or not name:
                continue
            m = re.search(r"^(?:bool|void)\s+" + re.escape(name) + r"\s*\(", text, re.M)
            if m:
                defs[name] = (f.name, extract_body(text, m.end()))
    for h in hooks:
        name = h.get("name", "")
        addr = int(h.get("address", 0))
        fname, body = defs.get(name, ("", ""))
        flow = any(k in h for k in ("return", "return_on_true", "return_on_false", "jump_address",
                                     "jump_address_on_true", "jump_address_on_false"))
        mutating = flow or bool(_HOOK_WRITE_RE.search(body))
        out[f"midasm:0x{addr:08X}"] = {
            "file": fname or "(hook function not found in kernel/*.cpp)",
            "gates": sorted(set(gates_in(body, {}))),
            "auto": "midasm" if mutating or not fname else "observe",
            "why": f"hook {name}" + (" (control flow)" if flow else "") + (" (writes state)" if _HOOK_WRITE_RE.search(body) else ""),
        }
    return out


def main():
    found = scan()
    existing = {}
    if MANIFEST.exists():
        existing = json.loads(MANIFEST.read_text(encoding="utf-8"))
    out = {}
    for sym, mech in sorted(found.items()):
        prev = existing.get(sym, {})
        entry = {
            "file": mech["file"],
            "gates": mech["gates"],
            "auto": mech["auto"],
            "class": prev.get("class")
            or ("observe" if mech["auto"] == "observe" else "needs-review"),
            "evidence": prev.get("evidence", ""),
            "retire": prev.get("retire", ""),
        }
        # R1: where the forced behaviour of a hook / R1 port acts (manual; audit_forcing.py
        # counts `live` and `conditional` band-aids as forcing that ships). See DEFAULT_EXE.
        if prev.get("default_exe"):
            entry["default_exe"] = prev["default_exe"]
        if "variants" in mech:
            entry["variants"] = mech["variants"]
            entry["gates_scope"] = mech["gates_scope"]
        # If the code regressed from observe to MUTATING, force re-review. The
        # test is deliberately "a mutation was found", not "auto != observe":
        # auto also goes non-observe on delegate COUNT (a hook that calls
        # __imp__ in two branches), which is shape, not behavior. Demoting on
        # that overrides a human who has already looked and written down why.
        mutates = "before:" in mech.get("why", "") or "after:" in mech.get("why", "")
        if entry["class"] == "observe" and mutates:
            entry["class"] = "needs-review"
        # iter-463 phase 2: `devtool` is mechanical in both directions - the exact pass-through
        # shape earns it, and losing the shape (or a hand-set devtool on any other body) demotes
        # it to needs-review.
        if mech["auto"] == "devtool":
            entry["class"] = "devtool"
        elif entry["class"] == "devtool":
            entry["class"] = "needs-review"
        # Symmetric, deliberately narrower promotion: a needs-review entry that
        # NO HUMAN ever annotated (no evidence, no retirement condition) and
        # that the mechanical check now reads as pure delegate-and-log goes back
        # to observe. Without this, an entry mis-defaulted by a scanner bug is
        # frozen as needs-review forever and the burndown number stops meaning
        # anything. A human-annotated entry is never promoted automatically.
        if (
            entry["class"] == "needs-review"
            and mech["auto"] == "observe"
            and not entry["evidence"]
            and not entry["retire"]
        ):
            entry["class"] = "observe"
        # \u26d4 iter-457 r5b: ALSO record the reason when the MECHANICAL verdict
        # is non-observe, even if a human has classified the entry `observe`.
        # The old condition keyed on `class` alone, so an entry the scanner had
        # flagged - `auto: needs-review`, `class: observe` - appeared in the
        # registry as a bare flag with NO stated reason. That is the one
        # combination where a reviewer most needs it, because it is exactly
        # where the human and the machine disagree.
        if (entry["class"] != "observe" or mech["auto"] != "observe") and mech.get("why"):
            entry["why"] = mech["why"]      # auto-derived, recomputed each run
        out[sym] = entry
    stale = sorted(set(existing) - set(found))
    MANIFEST.write_text(json.dumps(out, indent=1) + "\n", encoding="utf-8")
    counts = {}
    for e in out.values():
        counts[e["class"]] = counts.get(e["class"], 0) + 1
    print(f"overrides: {len(out)}  by class: {counts}")
    if stale:
        print(f"removed (no longer defined): {len(stale)}: {', '.join(stale[:8])}...")
    return 0


if __name__ == "__main__":
    sys.exit(main())
