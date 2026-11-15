#!/usr/bin/env python3
"""test_gen_override_manifest.py -- fixtures AND mutants for the override generator.

WHY THIS EXISTS. `gen_override_manifest.py` is the sole backstop for the rule
that no override lands invisibly (CLAUDE.md: a guest-side malloc substitution
once hid from the registry for dozens of sessions). It had NO test, and in
iter-457 it shipped two defects at once:

  1. It read a FORWARD DECLARATION as a strong definition. A declaration of
     `sub_82100310` in kernel/tex_unlock_test.cpp therefore made the TEST the
     recorded owner of a production override -- and because scan() walks files
     in sorted order with later writes winning, it overwrote the true owner.
     `audit_forcing.py` then passed on that: a FALSE NEGATIVE in the one file
     whose whole purpose is to make false negatives impossible.
  2. It classified a CONDITIONALLY delegated wrapper as pure `observe`, which
     asserts unconditional delegation.

And the first repair introduced a third: a scanner that could not tell code from
commentary dropped four REAL overrides whose `PPC_FUNC(...)` line carries a
trailing comment containing semicolons -- silently erasing four documented
BAND-AIDS, their evidence and their retirement conditions.

Every one of those three is a fixture below, and each is mutation-checked.

    python scripts/test_gen_override_manifest.py            # fixtures
    python scripts/test_gen_override_manifest.py --mutate   # + mutants

Exit 0 = all fixtures pass (and, with --mutate, every mutant is detected).
"""
import argparse, importlib.util, os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "scripts", "gen_override_manifest.py")

Q = chr(34)
NL = chr(10)


def load(path=GEN):
    spec = importlib.util.spec_from_file_location("gom_under_test", path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


# ── the fixtures, as (name, callable(mod) -> bool) ──────────────────────────
def fixtures(g):
    out = []

    def ck(ok, what):
        out.append((bool(ok), what))

    # ── 1. DECLARATION vs DEFINITION ────────────────────────────────────────
    decl = ("void sub_82100310(PPCContext& __restrict ctx, uint8_t* base);"
            + NL + NL + "void somethingElse() {" + NL + "  q();" + NL + "}")
    defn = "void sub_82100310(PPCContext& __restrict ctx, uint8_t* base) {" + NL + "  x();" + NL + "}"
    m = g.DEF_RE.search(decl)
    ck(m is not None,
       "DEF_RE still matches a forward declaration (it cannot tell them apart "
       "on its own - that is what is_definition is for)")
    ck(m and not g.is_definition(decl, m.end()),
       "KILLER a forward DECLARATION is not a definition")
    m2 = g.DEF_RE.search(defn)
    ck(m2 and g.is_definition(defn, m2.end()),
       "KILLER a real definition IS one")

    # ── 2. COMMENTS AND STRINGS MUST NOT TERMINATE THE SCAN ─────────────────
    # This shape dropped four documented band-aids.
    commented = ("PPC_FUNC(sub_826C2790)   // u8 table @0x82930780; len2,3: "
                 "t[c1]; len4-7: +t[c3]" + NL + "{" + NL + "  y();" + NL + "}")
    m3 = g.DEF_RE.search(commented)
    ck(m3 and g.is_definition(commented, m3.end()),
       "KILLER a definition whose PPC_FUNC line carries a trailing // comment "
       "CONTAINING SEMICOLONS is still a definition - reading those semicolons "
       "as a declaration terminator silently un-registered four band-aids")
    blockc = ("void sub_82ABCDEF(PPCContext& ctx, uint8_t* b) /* ; not a decl ; */"
              + NL + "{" + NL + "}")
    m4 = g.DEF_RE.search(blockc)
    ck(m4 and g.is_definition(blockc, m4.end()),
       "a /* block comment */ containing semicolons does not terminate it")
    # The string has to sit BEFORE the body brace to exercise the skip at
    # all - with `log(";")` inside the body, is_definition has already
    # returned at the opening `{` and the mutant survived. An attribute
    # between the parameter list and the brace is the shape that reaches it.
    strd = ("void sub_82ABCDE0(PPCContext& ctx, uint8_t* b)" + NL +
            "    __attribute__((annotate(" + Q + "; not a decl" + Q + ")))"
            + NL + "{" + NL + "  y();" + NL + "}")
    m5 = g.DEF_RE.search(strd)
    ck(m5 and g.is_definition(strd, m5.end()),
       "a string literal containing a SEMICOLON does not read as a "
       "declaration terminator")

    # \u26d4 THE SHAPE THAT REFUTED r5. I argued literal skipping was
    # unreachable - that any string between the match and the body must sit
    # inside parentheses, where the paren-depth guard reaches it first - and
    # deleted the skip because no fixture I could write killed its mutant. The
    # director produced this, and COMPILED IT: a C++20 trailing requires-clause
    # puts a literal at paren depth ZERO. The `;` inside it then terminates the
    # scan and a real definition is read as a declaration, dropping the override
    # from the registry entirely.
    #
    # The lesson is not about strings. "My mutant survived" was evidence my
    # FIXTURES were too narrow, and I read it as evidence the CODE was dead.
    #
    # Verified valid with:
    #   clang++ -std=c++20 -fsyntax-only <this source>   ->  exit 0
    req = ("template <fixed_string S> consteval bool operator" + Q + Q + "_ok()"
           " { return true; }" + NL +
           "template <class T>" + NL +
           "void sub_82ABCDE0(PPCContext& ctx, uint8_t* b)" + NL +
           "    requires " + Q + ";" + Q + "_ok" + NL +
           "{ (void)ctx; (void)b; }")
    m6 = g.DEF_RE.search(req)
    ck(m6 is not None,
       "DEF_RE matches the requires-clause definition (it cannot tell this "
       "from a declaration on its own)")
    ck(g.is_definition(req, m6.end()),
       "KILLER a valid C++20 trailing requires-clause holding a string literal "
       "with a SEMICOLON, at paren depth ZERO, is still a DEFINITION. This is "
       "the shape that refuted the r5 claim that literal skipping was "
       "unreachable")

    # ── 3. CONDITIONAL DELEGATION ───────────────────────────────────────────
    uncond = "{ prologue(); __imp__sub_820F0000(ctx, base); tail(); }"
    ck(not g.delegate_conditional("sub_820F0000", uncond),
       "an UNCONDITIONAL delegate call is not conditional")
    ck(g.classify_auto("sub_820F0000", uncond) == "observe",
       "...and it classifies as observe")

    guarded = ("{ prologue();" + NL +
               "  if (!bypass) __imp__sub_820F0000(ctx, base);" + NL +
               "  tail(); }")
    ck(g.delegate_conditional("sub_820F0000", guarded),
       "KILLER an `if (...)` guarded delegate call IS conditional")
    ck(g.classify_auto("sub_820F0000", guarded) == "needs-review",
       "KILLER ...and it is NOT classified as pure observe. `observe` asserts "
       "UNCONDITIONAL delegation; a wrapper that can skip the guest body is a "
       "materially different contract")

    nested = ("{ if (a) {" + NL + "    __imp__sub_820F0000(ctx, base);" + NL +
              "  } }")
    ck(g.delegate_conditional("sub_820F0000", nested),
       "KILLER a delegate call NESTED inside a block is conditional")

    tern = "{ a ? __imp__sub_820F0000(ctx, base) : nop(); }"
    ck(g.delegate_conditional("sub_820F0000", tern),
       "a ternary-guarded delegate call is conditional")

    commented_if = ("{ prologue();   // if (x) this is only a comment" + NL +
                    "  __imp__sub_820F0000(ctx, base); }")
    ck(not g.delegate_conditional("sub_820F0000", commented_if),
       "KILLER an `if` inside a COMMENT does not make it conditional - the "
       "detector reads code, not prose")

    # ── 3b. GOVERNED SCOPE, not raw depth──────────
    # \u26d4 THE DEFECT r5 SHIPPED. The first detector asked how DEEP the call was
    # nested. `try { __imp__X(...); }` is nested and completely unconditional -
    # and that shape demoted a real override (sub_8235F488, an exception probe
    # around the FMV state-machine tick) to needs-review on a false premise. A
    # registry that cries wolf dilutes the burndown list the charter depends on.
    tryw = ("{" + NL + "  try {" + NL + "    __imp__sub_820F0000(ctx, base);" +
            NL + "  } catch (...) {" + NL + "    note();" + NL + "    throw;" +
            NL + "  }" + NL + "}")
    ck(g.delegate_scope("sub_820F0000", tryw) == "",
       "KILLER a delegate inside `try { }` is NOT conditional - it always runs, "
       "and the previous depth test called it conditional (got %r)"
       % g.delegate_scope("sub_820F0000", tryw))
    ck(g.classify_auto("sub_820F0000", tryw) == "observe",
       "KILLER ...so it classifies as observe, not needs-review")

    catchw = ("{" + NL + "  try {" + NL + "    risky();" + NL +
              "  } catch (...) {" + NL + "    __imp__sub_820F0000(ctx, base);" +
              NL + "  }" + NL + "}")
    ck(g.delegate_scope("sub_820F0000", catchw) == "conditional",
       "KILLER ...but a delegate inside the CATCH block IS conditional - it "
       "runs only on an exception (got %r)"
       % g.delegate_scope("sub_820F0000", catchw))

    scopew = ("{" + NL + "  {" + NL + "    __imp__sub_820F0000(ctx, base);" +
              NL + "  }" + NL + "}")
    ck(g.delegate_scope("sub_820F0000", scopew) == "",
       "a bare nested scope is not a condition either")

    lam = ("{" + NL + "  dispatch(ctx, base," + NL +
           "      [](PPCContext& c, uint8_t* b) { __imp__sub_820F0000(c, b); },"
           + NL + "      enabled());" + NL + "}")
    ck(g.delegate_scope("sub_820F0000", lam) == "indeterminate",
       "KILLER a delegate inside a LAMBDA is INDETERMINATE, not conditional - "
       "whether it runs is the callee's decision and is not visible here "
       "(got %r)" % g.delegate_scope("sub_820F0000", lam))
    ck("LAMBDA" in g.why_flagged("sub_820F0000", lam)
       and "can be skipped" not in g.why_flagged("sub_820F0000", lam),
       "KILLER ...and the recorded reason SAYS indeterminate rather than "
       "claiming the guest body can be skipped, which the scanner never "
       "established (got %r)" % g.why_flagged("sub_820F0000", lam))
    ck(g.classify_auto("sub_820F0000", lam) != "observe",
       "...while still leaving observe, because observe asserts UNCONDITIONAL "
       "delegation and that was not established either")

    unbraced = ("{" + NL + "  if (!bypass()) __imp__sub_820F0000(ctx, base);" +
                NL + "}")
    ck(g.delegate_scope("sub_820F0000", unbraced) == "conditional",
       "KILLER an UNBRACED if-guard is conditional - this is the shape the "
       "shipped wrapper actually uses, and no brace nesting exists to notice it")

    tern = ("{" + NL +
            "  on() ? __imp__sub_820F0000(ctx, base) : fallback(ctx);" + NL + "}")
    ck(g.delegate_scope("sub_820F0000", tern) == "conditional",
       "KILLER a TERNARY branch is conditional - the `?` sits before the call "
       "and the `:` after it, so looking for both around the call was wrong")

    andand = ("{" + NL + "  ready() && (__imp__sub_820F0000(ctx, base), true);" +
              NL + "}")
    ck(g.delegate_scope("sub_820F0000", andand) == "conditional",
       "a short-circuit `&&` guard is conditional too")

    plain = ("{" + NL + "  prologue();" + NL +
             "  __imp__sub_820F0000(ctx, base);" + NL + "  epilogue();" + NL + "}")
    ck(g.delegate_scope("sub_820F0000", plain) == "",
       "CONTROL a plain top-level delegate is unconditional - the whole point "
       "of the classifier is that this stays `observe`")
    ck(g.classify_auto("sub_820F0000", plain) == "observe",
       "CONTROL ...and it classifies as observe")

    # \u26d4 THE CONTRACT that makes stack[1:] and stack[0:] equivalent, pinned
    # here so it cannot rot silently: extract_body returns text INCLUDING the
    # outer braces, so a body always starts with `{`, and that brace has no
    # governor. If this ever stops holding, the retired outer-brace mutant
    # becomes killable again and must be restored.
    _probe = "void sub_820F0000(PPCContext& ctx, uint8_t* b) { a(); { b(); } }"
    _mm = g.DEF_RE.search(_probe)
    ck(g.extract_body(_probe, _mm.end()).startswith("{"),
       "KILLER extract_body returns text STARTING with the function's own "
       "brace - the contract under which the outer brace has no governor")
    ck(g._governor(g.extract_body(_probe, _mm.end()), 0) == "",
       "KILLER ...and that brace's governor is the empty string, which is why "
       "including or excluding it cannot change any verdict")

    ifblock = ("{" + NL + "  if (on()) {" + NL +
               "    __imp__sub_820F0000(ctx, base);" + NL + "  }" + NL + "}")
    ck(g.delegate_scope("sub_820F0000", ifblock) == "conditional",
       "KILLER an if-BLOCK is still conditional after the rewrite - the fix "
       "for the false positive must not create a false negative")

    # ── 4. scan() END TO END - the shape of the real defect ─────────────────
    # \u26d4 The two helpers above can both be correct while scan() still records
    # the wrong owner, because scan() walks files in SORTED order and later
    # writes win. That is exactly what happened: `hle_api_observe.cpp` defines
    # sub_82100310, `tex_unlock_test.cpp` merely DECLARES it, and the test file
    # sorts later - so the registry named the test as the owner, with no gates,
    # and audit_forcing.py passed on it. No fixture that only calls is_definition
    # can see this.
    import tempfile as _tf, pathlib as _pl, json as _json
    with _tf.TemporaryDirectory() as _d:
        _root = _pl.Path(_d)
        (_root / "kernel").mkdir()
        (_root / "guest").mkdir()
        (_root / "kernel" / "aaa_real_owner.cpp").write_text(
            "void sub_82100310(PPCContext& __restrict ctx, uint8_t* base) {" + NL +
            '  if (getenv("SR_FOO")) note();' + NL +
            "  __imp__sub_82100310(ctx, base);" + NL +
            "}" + NL, encoding="utf-8")
        (_root / "kernel" / "zzz_only_declares.cpp").write_text(
            "// a fixture calls the real override, so it declares it" + NL +
            "void sub_82100310(PPCContext& __restrict ctx, uint8_t* base);" + NL +
            "void unrelated() { sub_82100310(c, b); }" + NL, encoding="utf-8")
        _oldK, _oldR = g.KERNEL, g.ROOT
        g.KERNEL, g.ROOT = _root / "kernel", _root
        try:
            _found = g.scan()
        finally:
            g.KERNEL, g.ROOT = _oldK, _oldR
    _e = _found.get("sub_82100310", {})
    ck(_e.get("file") == "aaa_real_owner.cpp",
       "KILLER scan() records the file that DEFINES the override, not the one "
       "that merely declares it - even though the declaring file sorts LATER "
       "and later writes win (got %r)" % _e.get("file"))
    ck(_e.get("gates") == ["SR_FOO"],
       "KILLER ...and it keeps the real owner's GATES. The mis-attributed entry "
       "had gates [], which is what audit_forcing.py reads to decide whether a "
       "band-aid is active by default (got %r)" % _e.get("gates"))

    # ── iter-463 round 4: the HAND-WRITTEN guest stubs are overrides too ─────
    # tools/regen/hand/ppc_missing_recomp_stubs.cpp is compiled into the guest
    # library; its PPC_FUNC strong defs replace recompiled weak bodies and its
    # PPC_MISSING_STUB(sub_X) macros define empty ones. The registry never read
    # it, so none was classified.
    with _tf.TemporaryDirectory() as _d4:
        _r4 = _pl.Path(_d4)
        (_r4 / "kernel").mkdir()
        (_r4 / "guest").mkdir()
        (_r4 / "tools" / "regen" / "hand").mkdir(parents=True)
        (_r4 / "tools" / "regen" / "hand" / "ppc_missing_recomp_stubs.cpp").write_text(
            "#define PPC_MISSING_STUB(name) PPC_FUNC(name) { (void)ctx; (void)base; }" + NL +
            "PPC_MISSING_STUB(sub_828D0EC8)" + NL +
            "PPC_FUNC(sub_824B9560) { ctx.r3.u32 = 0; }" + NL, encoding="utf-8")
        _oK4, _oR4 = g.KERNEL, g.ROOT
        g.KERNEL, g.ROOT = _r4 / "kernel", _r4
        try:
            _f4 = g.scan()
        finally:
            g.KERNEL, g.ROOT = _oK4, _oR4
    ck(_f4.get("sub_824B9560", {}).get("file") == "ppc_missing_recomp_stubs.cpp"
       and _f4["sub_824B9560"].get("auto") == "needs-review",
       "KILLER scan() registers a hand-written guest stub's PPC_FUNC override "
       "(a fabricated return) as needs-review (got %r)" % _f4.get("sub_824B9560"))
    ck(_f4.get("sub_828D0EC8", {}).get("via_macro") == "PPC_MISSING_STUB",
       "KILLER ...and its PPC_MISSING_STUB(sub_X) empty stubs (got %r)" % _f4.get("sub_828D0EC8"))


    # \u26d4 main() END TO END - the why-suppression wart. scan() cannot see this
    # one either: the reason is attached in main(), and the old rule keyed on
    # `class` alone, so an entry with `auto: needs-review` and `class: observe`
    # reached the registry as a BARE FLAG with no stated reason - precisely the
    # combination where the human and the machine disagree and a reviewer most
    # needs to know why.
    with _tf.TemporaryDirectory() as _d2:
        _r2 = _pl.Path(_d2)
        (_r2 / "kernel").mkdir()
        (_r2 / "kernel" / "lam.cpp").write_text(
            "void sub_82ABCD00(PPCContext& __restrict ctx, uint8_t* base) {" + NL +
            "  dispatch(ctx, base," + NL +
            "      [](PPCContext& c, uint8_t* b) { __imp__sub_82ABCD00(c, b); });"
            + NL + "}" + NL, encoding="utf-8")
        _man = _r2 / "OVERRIDES.json"
        _man.write_text(_json.dumps({"sub_82ABCD00": {
            "file": "lam.cpp", "gates": [], "auto": "observe",
            "class": "observe", "evidence": "", "retire": ""}}), encoding="utf-8")
        _oK, _oR, _oM = g.KERNEL, g.ROOT, g.MANIFEST
        g.KERNEL, g.ROOT, g.MANIFEST = _r2 / "kernel", _r2, _man
        try:
            # main() prints a summary; the suite's output stays its own.
            import contextlib as _cl, io as _io
            with _cl.redirect_stdout(_io.StringIO()):
                g.main()
            _written = _json.loads(_man.read_text(encoding="utf-8"))
        finally:
            g.KERNEL, g.ROOT, g.MANIFEST = _oK, _oR, _oM
    _we = _written.get("sub_82ABCD00", {})
    ck(_we.get("auto") == "needs-review" and _we.get("class") == "observe",
       "the fixture really does produce the disagreeing combination "
       "(auto=%r class=%r)" % (_we.get("auto"), _we.get("class")))
    ck(bool(_we.get("why")),
       "KILLER an entry the scanner FLAGGED carries its stated reason into the "
       "registry even though a human classified it `observe` - a bare flag with "
       "no reason is unactionable (got why=%r)" % _we.get("why"))

    # \u26d4 FLAG-MEDIATED GATES - director defect 2. An override can be gated
    # without ever naming its gate: it reads a file-scope flag, and the getenv
    # lives in the SETTER that writes it. The registry then recorded
    # `gates: []` while the prose claimed structural gating - and `gates` is
    # what audit_forcing.py reads to decide whether a classified band-aid runs
    # by default, so an empty list there is a false statement about the code.
    with _tf.TemporaryDirectory() as _d3:
        _r3 = _pl.Path(_d3)
        (_r3 / "kernel").mkdir()
        (_r3 / "kernel" / "flagged.cpp").write_text(
            "std::atomic<bool> s_bypass{false};" + NL +
            NL +
            # A second function whose own LOCAL is also called `v`, and
            # which carries a different gate. This is what the real defect
            # looked like: the name `v` appears in many gated helpers, so
            # treating it as a file-scope flag drags all their gates in.
            "bool otherHelper() {" + NL +
            "  static const bool v = std::getenv(" + Q + "SR_NOISE" + Q +
            ") != nullptr;" + NL +
            "  return v;" + NL +
            "}" + NL +
            NL +
            "bool armBypassForTest(bool on) {" + NL +
            "  static const bool inSuite = [] {" + NL +
            "    const char* e = std::getenv(" + Q + "SR_ARM_GATE" + Q + ");" + NL +
            "    return e && e[0] == '1';" + NL +
            "  }();" + NL +
            "  if (!inSuite) { s_bypass.store(false); return false; }" + NL +
            "  s_bypass.store(on);" + NL +
            "  return true;" + NL +
            "}" + NL +
            NL +
            "void sub_82CAFE00(PPCContext& __restrict ctx, uint8_t* base) {" + NL +
            "  static const bool v = true;   // a LOCAL static, not a flag" + NL +
            "  (void)v;" + NL +
            "  if (!s_bypass.load()) __imp__sub_82CAFE00(ctx, base);" + NL +
            "  note();" + NL +
            "}" + NL, encoding="utf-8")
        _oK3, _oR3 = g.KERNEL, g.ROOT
        g.KERNEL, g.ROOT = _r3 / "kernel", _r3
        try:
            _f3 = g.scan()
        finally:
            g.KERNEL, g.ROOT = _oK3, _oR3
    _g3 = _f3.get("sub_82CAFE00", {}).get("gates")
    ck(_g3 == ["SR_ARM_GATE"],
       "KILLER an override that only READS a flag reports the gate of the "
       "function that WRITES it - the registry said `gates: []` for exactly "
       "this shape while the prose claimed structural gating (got %r)" % _g3)
    ck(_g3 == ["SR_ARM_GATE"] and "SR_NOISE" not in (_g3 or []),
       "KILLER ...and the function-LOCAL `static const bool v` is not mistaken "
       "for a file-scope flag. When it was, one override collected FOURTEEN "
       "gates it does not have, and a fabricated gate can make a live band-aid "
       "look switched off (got %r)" % _g3)

    # \u26d4 H1a-15 / H1a-16 - the two flag_gates counterexamples the director
    # constructed. r6 attributed EVERY gate found anywhere in a writer function,
    # which invents gates; and it required column 0 for non-static flags, which
    # misses real ones. Over-reporting is the dangerous direction, because
    # audit_forcing.py reads `gates` to decide whether a classified band-aid runs
    # by default - a fabricated gate makes a live band-aid look switched off.
    fake = ("static std::atomic<bool> s_flag{false};" + NL +
            "void writer() {" + NL +
            "  if (std::getenv(" + Q + "SR_FAKE" + Q + ")) note();" + NL +
            "  s_flag.store(true);" + NL +
            "}" + NL +
            "void sub_82CAFE01(PPCContext& ctx, uint8_t* b) {" + NL +
            "  if (!s_flag.load()) __imp__sub_82CAFE01(ctx, b);" + NL + "}" + NL)
    _fm = [m for m in g.DEF_RE.finditer(fake) if g.is_definition(fake, m.end())]
    _fb = g.extract_body(fake, _fm[0].end())
    ck(sorted(g.flag_gates(_fb, fake, {})) == [],
       "KILLER H1a-15 an unrelated getenv in a writer does NOT become the "
       "flag's gate - the write is unconditional, so nothing gates it (got %r)"
       % sorted(g.flag_gates(_fb, fake, {})))

    hid = ("namespace hidden {" + NL +
           "  bool s_flag = false;" + NL +
           "  void writer() { s_flag = std::getenv(" + Q + "SR_REAL" + Q +
           ") != nullptr; }" + NL +
           "}" + NL +
           "void sub_82CAFE02(PPCContext& ctx, uint8_t* b) {" + NL +
           "  if (!hidden::s_flag) __imp__sub_82CAFE02(ctx, b);" + NL + "}" + NL)
    _hm = [m for m in g.DEF_RE.finditer(hid) if g.is_definition(hid, m.end())]
    _hb = g.extract_body(hid, _hm[0].end())
    ck(sorted(g.flag_gates(_hb, hid, {})) == ["SR_REAL"],
       "KILLER H1a-16 an INDENTED namespace-scope flag is found, and the gate "
       "it is assigned FROM is reported. Requiring column 0 missed this "
       "entirely, with no pointer or second-level indirection involved (got %r)"
       % sorted(g.flag_gates(_hb, hid, {})))

    latched = ("static std::atomic<bool> s_arm{false};" + NL +
               "bool armFor(bool on) {" + NL +
               "  static const bool inSuite = [] {" + NL +
               "    const char* e = std::getenv(" + Q + "SR_ARM" + Q + ");" + NL +
               "    return e && e[0] == '1';" + NL +
               "  }();" + NL +
               "  if (!inSuite) { s_arm.store(false); return false; }" + NL +
               "  s_arm.store(on);" + NL +
               "  return true;" + NL + "}" + NL +
               "void sub_82CAFE03(PPCContext& ctx, uint8_t* b) {" + NL +
               "  if (!s_arm.load()) __imp__sub_82CAFE03(ctx, b);" + NL + "}" + NL)
    _lm = [m for m in g.DEF_RE.finditer(latched) if g.is_definition(latched, m.end())]
    _lb = g.extract_body(latched, _lm[0].end())
    ck(sorted(g.flag_gates(_lb, latched, {})) == ["SR_ARM"],
       "CONTROL the LATCHED-GUARD shape still reports its gate - this is the "
       "shipped sub_82100310 setter, and narrowing the rule must not lose it "
       "(got %r)" % sorted(g.flag_gates(_lb, latched, {})))

    # \u26d4 H1a-17 - a valid C++20 RAW literal. Its payload is NOT
    # escape-processed, so the `"` inside ended the ordinary-literal skip early
    # and the `;` after it terminated the scan. Verified valid with:
    #   clang++ -std=c++20 -fsyntax-only   ->  exit 0
    rawreq = ("template <fixed_string S> consteval bool operator" + Q + Q +
              "_ok() { return true; }" + NL +
              "template <class T>" + NL +
              "void sub_82ABCDE1(PPCContext& ctx, uint8_t* b)" + NL +
              "    requires R" + Q + "tag(" + Q + ";{)tag" + Q + "_ok" + NL +
              "{ (void)ctx; (void)b; }")
    _rm = g.DEF_RE.search(rawreq)
    ck(_rm is not None, "DEF_RE matches the raw requires-clause definition")
    ck(g.is_definition(rawreq, _rm.end()),
       "KILLER H1a-17 a valid C++20 RAW string literal holding a quote, a "
       "semicolon and a brace does not turn a DEFINITION into a declaration")

    # \u26d4 H1a-18 - do-while runs its body at least once.
    dow = ("{" + NL + "  do { __imp__sub_82CCCC00(ctx, base); } while (again());"
           + NL + "}")
    ck(g.delegate_scope("sub_82CCCC00", dow) == "",
       "KILLER H1a-18 a delegate in a do-while BODY is unconditional - the "
       "body always runs at least once (got %r)"
       % g.delegate_scope("sub_82CCCC00", dow))
    ck(g.classify_auto("sub_82CCCC00", dow) == "observe",
       "KILLER H1a-18 ...so it classifies as observe, not needs-review")
    dowif = ("{" + NL +
             "  do { if (on()) __imp__sub_82CCCC00(ctx, base); } while (again());"
             + NL + "}")
    ck(g.delegate_scope("sub_82CCCC00", dowif) == "conditional",
       "KILLER H1a-18 ...but a REAL condition nested inside the do-body is "
       "still found - removing the false positive must not create a false "
       "negative")

    # \u26d4 A CLASS MEMBER IS NOT A FILE-SCOPE FLAG. Relaxing the indentation
    # rule for H1a-16 immediately created the opposite fault: `struct Foo { bool
    # live; };` registered `live`, and SEVEN real overrides that merely mention a
    # member of that name collected SR_S6_WINFIX. Caught by regenerating and
    # reading the delta rather than by assuming the fix was clean.
    memb = ("struct Foo {" + NL + "  bool live;" + NL + "};" + NL +
            "static const bool s_on = [] {" + NL +
            "  return std::getenv(" + Q + "SR_MEMBER" + Q + ") != nullptr;" + NL +
            "}();" + NL +
            "void writer(Foo& f) {" + NL +
            "  if (s_on) f.live = true;" + NL + "}" + NL +
            "void sub_82CAFE04(PPCContext& ctx, uint8_t* b) {" + NL +
            "  bool live = false; (void)live;" + NL +
            "  __imp__sub_82CAFE04(ctx, b);" + NL + "}" + NL)
    ck("live" not in g._file_scope_flags(memb),
       "KILLER a struct MEMBER is not a file-scope flag - treating one as a "
       "flag gave seven real overrides a gate none of them has (flags were %r)"
       % sorted(g._file_scope_flags(memb)))
    _mm4 = [m for m in g.DEF_RE.finditer(memb) if g.is_definition(memb, m.end())]
    _mb4 = g.extract_body(memb, _mm4[-1].end())
    ck(sorted(g.flag_gates(_mb4, memb, {})) == [],
       "KILLER ...so no gate is fabricated for a wrapper that merely uses that "
       "name (got %r)" % sorted(g.flag_gates(_mb4, memb, {})))

    none = "{ doItOurselves(); }"
    ck(not g.delegate_conditional("sub_820F0000", none),
       "a body that never delegates is not 'conditional' (it is a full "
       "replacement, which why_flagged reports separately)")

    # ── iter-463 phase 2: the `devtool` class (docs/DEVTOOLS_DESIGN.md 2.2) ──
    # Earned ONLY by the exact gate-off pass-through shape; anything else that
    # names a devToolHookNeeded_* predicate is needs-review.
    dt_ok = ("{" + NL + "  if (!sr::kernel::devToolHookNeeded_sub_8223EB08()) {" + NL +
             "    __imp__sub_8223EB08(ctx, base);" + NL + "    return;" + NL + "  }" + NL +
             "  onTick(base, true);" + NL + "  __imp__sub_8223EB08(ctx, base);" + NL + "}")
    ck(g.classify_auto("sub_8223EB08", dt_ok) == "devtool",
       "devtool: the exact pass-through shape is class devtool (got %r)" % g.classify_auto("sub_8223EB08", dt_ok))
    dt_reg = dt_ok.replace("  onTick(base, true);", "  ctx.r3.u32 = 0;")
    ck(g.classify_auto("sub_8223EB08", dt_reg) == "needs-review",
       "devtool: a guest register write anywhere in the body demotes it to needs-review")
    dt_late = ("{" + NL + "  onTick(base, true);" + NL +
               "  if (!devToolHookNeeded_sub_8223EB08()) { __imp__sub_8223EB08(ctx, base); return; }" + NL +
               "  __imp__sub_8223EB08(ctx, base);" + NL + "}")
    ck(g.classify_auto("sub_8223EB08", dt_late) == "needs-review",
       "devtool: work BEFORE the gate (not an exact pass-through when off) is needs-review")
    dt_other = dt_ok.replace("devToolHookNeeded_sub_8223EB08", "devToolHookNeeded_sub_8223ECC0")
    ck(g.classify_auto("sub_8223EB08", dt_other) == "needs-review",
       "devtool: the predicate must be the one named after this function")
    dt_noreturn = dt_ok.replace("    return;" + NL, "")
    ck(g.classify_auto("sub_8223EB08", dt_noreturn) == "needs-review",
       "devtool: the gate-off branch must return right after __imp__ (else the hook runs too)")
    ck("devtool hook: exact pass-through" in g.why_flagged("sub_8223EB08", dt_ok),
       "devtool: why_flagged states the shape")
    return out


def run_fixtures(path=GEN, quiet=False):
    g = load(path)
    res = fixtures(g)
    npass = sum(1 for ok, _ in res if ok)
    if not quiet:
        for ok, what in res:
            print(("  PASS  " if ok else "  FAIL  ") + what)
        print(NL + "%d/%d override-generator fixtures behave as specified"
              % (npass, len(res)))
    return len(res) - npass


MUTANTS = [
    # iter-463 phase 2: the devtool class's shape check (docs/DEVTOOLS_DESIGN.md 2.2)
    ("devtool shape: the register-write check is dropped",
     "    return True, bool(head) and not MUTATION_RE.search(clean)",
     "    return True, bool(head)"),

    ("devtool shape: any devToolHookNeeded predicate earns the class",
     '        return "devtool" if ok else "needs-review"',
     '        return "devtool"'),

    ("devtool shape: any devToolHookNeeded_* predicate name is accepted",
     r'(?:sr\s*::\s*kernel\s*::\s*)?devToolHookNeeded_" + sym +',
     r'(?:sr\s*::\s*kernel\s*::\s*)?devToolHookNeeded_\w+" + "" +'),

    ("devtool shape: the return after the gate-off delegate is optional",
     r'\s*\(\s*ctx\s*,\s*base\s*\)\s*;\s*return\s*;\s*\}", clean)',
     r'\s*\(\s*ctx\s*,\s*base\s*\)\s*;\s*(?:return\s*;)?\s*\}", clean)'),

    # Anchors are the LIVE text. They were first written against an earlier
    # shape of the code and six of nine failed to match - which the harness
    # reported as an anchor error rather than a pass, exactly as it should.
    ("is_definition treats a declaration as a definition",
     '            if c == ";":' + NL + "                return False",
     "            if False:" + NL + "                return False"),

    # The same two lines open the skip in strip_comments_and_strings, so the
    # anchor carries a third line - "i += 1" here, "j = i + 1" there - to
    # name which function is under mutation.
    ("is_definition does not skip string literals",
     '        if c in ("' + chr(39) + '", chr(34)):' + NL + "            q = c" + NL +
     "            i += 1",
     "        if False:" + NL + "            q = c" + NL + "            i += 1"),

    ("is_definition does not skip line comments",
     'if c == "/" and i + 1 < n and text[i + 1] == "/":',
     "if False:"),

    ("is_definition does not skip block comments",
     'if c == "/" and i + 1 < n and text[i + 1] == "*":',
     "if False:"),

    # \u26d4 A NOTE ON WHAT USED TO BE WRITTEN HERE. r5 removed the
    # string-literal mutant and argued the skip was unreachable: `;`/`{` are
    # only honoured at paren depth <= 0, so any string between the match and the
    # body must already be inside parentheses. The director refuted it with two
    # COMPILED counterexamples - a trailing requires-clause puts an ordinary
    # literal at depth zero, and a RAW literal's payload is not
    # escape-processed at all. Both mutants are live above and both die.
    #
    # The argument is preserved only as the thing that was WRONG. A surviving
    # mutant is evidence the fixtures are too narrow before it is ever evidence
    # the code is dead.

    ("delegate_scope ignores the governing keyword of every open block",
     "        if _is_conditional_governor(gov):" + NL +
     '            return "conditional"',
     "        if False:" + NL + '            return "conditional"'),

    ("a lambda-wrapped delegate is silently treated as unconditional",
     '            verdict = "indeterminate"',
     '            verdict = ""'),

    ("a catch block is not recognised as conditional",
     'if re.search(r"\\b(switch|catch)\\b", gov):',
     'if re.search(r"\\b(switch)\\b", gov):'),

    ("the depth test comes back, so `try` is conditional again",
     "    for brace_at in stack[1:]:" + NL + "        gov = _governor(head, brace_at)",
     "    if len(stack) > 1:" + NL + '        return "conditional"' + NL +
     "    for brace_at in stack[1:]:" + NL + "        gov = _governor(head, brace_at)"),

    ("delegate_scope ignores a short-circuit governor",
     '    return "?" in gov or "&&" in gov or "||" in gov',
     '    return "?" in gov'),

    # One mutant now covers unbraced `if`, ternary and short-circuit, because
    # one line decides all three. Its fixtures assert them separately.
    ("delegate_scope ignores the statement-level governor",
     "    if _is_conditional_governor(stmt):" + NL + '        return "conditional"',
     "    if False:" + NL + '        return "conditional"'),

    ("delegate_conditional does not strip comments (an `if` in prose counts)",
     "head = strip_comments_and_strings(body[:m.start()])",
     "head = body[:m.start()]"),

    # \u26a0 RETIRED: "the walk forgets to skip the body's own outer brace"
    # (stack[1:] -> stack[0:]). It SURVIVED, and the reason is structural, not a
    # fixture gap: `extract_body` returns text that STARTS with the function's
    # own `{`, so that brace sits at index 0 and its governor - everything back
    # to a previous boundary that does not exist - is always the empty string,
    # which governs nothing. Both slices therefore behave identically for every
    # input the contract admits.
    #
    # Unlike the retired string-skip mutant, NO CODE WAS DELETED here: one of
    # the two tokens has to be written, and `[1:]` is the one that states the
    # intent (the function's own brace is not a governed scope). The fixture
    # below pins the contract that makes them equivalent, so a reader who
    # changes extract_body sees what they are relying on.

    ("conditional delegation is classified as pure observe again",
     "if imp_calls == 1 and not mutated and not delegate_conditional(sym, body):",
     "if imp_calls == 1 and not mutated:"),

    ("a flagged entry loses its stated reason when class says observe",
     '        if (entry["class"] != "observe" or mech["auto"] != "observe") and mech.get("why"):',
     '        if entry["class"] != "observe" and mech.get("why"):'),

    # The macro path has the identical two lines, so the anchor carries the
    # line that follows only the LITERAL-definition path.
    ("the flag-to-gate wiring is removed from scan()",
     '                "gates": sorted(set(gates_in(body, helpers))' + NL +
     "                                | flag_gates(body, text, helpers))," + NL +
     '                "auto": classify_auto(sym, body),' + NL +
     '                "why": why_flagged(sym, body),' + NL +
     "            }" + NL + "            record_variant(variants, sym, found[sym], text, m.start(), locations)" + NL + "        # Macro-generated strong-defs (see collect_macros).",
     '                "gates": gates_in(body, helpers),' + NL +
     '                "auto": classify_auto(sym, body),' + NL +
     '                "why": why_flagged(sym, body),' + NL +
     "            }" + NL + "            record_variant(variants, sym, found[sym], text, m.start(), locations)" + NL + "        # Macro-generated strong-defs (see collect_macros)."),

    ("a function-local static counts as a file-scope flag",
     "        if any(b0 <= m.start() < b1 for b0, b1, _n, _b in spans):" + NL +
     "            continue",
     "        if False:" + NL + "            continue"),

    ("flag_gates attributes every gate in the writer, not just the governing one",
     "            out |= set(GATE_RE.findall(_write_statement(fbody, rel)))",
     "            out |= set(gates_in(fbody, helpers))"),

    ("flag_gates loses the latched-guard shape",
     "                if _latch_guards_write(fbody, latch, rel):" + NL +
     "                    out.add(gate)",
     "                if False:" + NL + "                    out.add(gate)"),

    ("file-scope flag discovery requires column 0 again",
     'r"|^\\s*" + _FLAG_TYPE + r"\\s+(\\w+)\\s*[{=;]",',
     'r"|^" + _FLAG_TYPE + r"\\s+(\\w+)\\s*[{=;]",'),

    ("FUNC_DEF_RE cannot see an indented definition",
     'r"^\\s*(?!(?:if|for|while|switch|catch|return|else|do)\\b)"',
     'r"^(?!(?:if|for|while|switch|catch|return|else|do)\\b)"'),

    ("is_definition does not skip RAW literals",
     "        raw = _raw_literal_end(text, i)" + NL +
     "        if raw is not None:",
     "        raw = None" + NL + "        if raw is not None:"),

    ("`do` is conditional again",
     'if re.search(r"\\b(switch|catch)\\b", gov):',
     'if re.search(r"\\b(switch|catch|do)\\b", gov):'),

    ("a class member counts as a file-scope flag",
     "        if any(b0 <= m.start() < b1 for b0, b1 in _type_spans(text)):" + NL +
     "            continue",
     "        if False:" + NL + "            continue"),

    ("scan() no longer reads the hand-written guest stubs (round 4)",
     "    scan_files += [ROOT / p for p in HAND_OVERRIDE_SOURCES if (ROOT / p).is_file()]",
     "    scan_files += []"),

    ("scan() no longer skips forward declarations",
     "if not is_definition(text, m.end()):" + NL + "                continue",
     "if False:" + NL + "                continue"),
]


def to_file_eol(text: str, live: str) -> str:
    """Anchors are written with NL; the file on disk may be CRLF.

    Four multi-line mutants reported "anchor appears 0 times" purely because of
    this. The harness said so plainly rather than scoring them as passes, which
    is the behaviour that matters - but a mutation suite that cannot find its
    own anchors measures nothing, so normalise here.
    """
    if chr(13) + chr(10) in live and chr(13) not in text:
        return text.replace(chr(10), chr(13) + chr(10))
    return text


def run_mutants():
    live = open(GEN, encoding="utf-8", newline="").read()
    base = run_fixtures(quiet=True)
    print("control: %d fixture failure(s)" % base)
    if base != 0:
        print("FAIL: the control must be clean. Every comparison below would be "
              "meaningless.")
        return 1
    print(NL + "%d mutants:" % len(MUTANTS) + NL)
    ok = True
    with tempfile.TemporaryDirectory() as d:
        mpath = os.path.join(d, "gen_override_manifest.py")
        for name, find, repl in MUTANTS:
            find = to_file_eol(find, live)
            repl = to_file_eol(repl, live)
            if live.count(find) != 1:
                print("  FAIL  %-64s anchor appears %d times (expected 1)"
                      % (name[:64], live.count(find)))
                ok = False
                continue
            open(mpath, "w", encoding="utf-8", newline="").write(
                live.replace(find, repl, 1))
            try:
                fails = run_fixtures(mpath, quiet=True)
            except Exception as e:
                print("  FAIL  %-64s raised %s (proves nothing)"
                      % (name[:64], type(e).__name__))
                ok = False
                continue
            if fails == 0:
                print("  FAIL  %-64s SURVIVED" % name[:64])
                ok = False
            else:
                print("  PASS  %-64s -> %d failed" % (name[:64], fails))
    print(NL + "OVERRIDE-GENERATOR MUTATION: %s"
          % ("every mutant is detected" if ok
             else "SOME MUTANTS SURVIVED - the fixtures are blind to them"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split(NL)[0])
    ap.add_argument("--mutate", action="store_true")
    a = ap.parse_args()
    rc = run_fixtures()
    if rc:
        return 1
    if a.mutate:
        return run_mutants()
    return 0


if __name__ == "__main__":
    sys.exit(main())
