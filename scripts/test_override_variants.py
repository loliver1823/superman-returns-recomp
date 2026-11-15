"""Conditional definition inventory controls; no active-build acceptance claims.

SR_OVERRIDE_GENERATOR/SR_OVERRIDE_ROOT allow an immutable candidate script to be
tested against a pinned worker source without modifying that source tree.
"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(os.environ.get('SR_OVERRIDE_ROOT', Path(__file__).resolve().parents[1]))
GEN = Path(os.environ.get('SR_OVERRIDE_GENERATOR', ROOT / 'scripts/gen_override_manifest.py'))
spec = importlib.util.spec_from_file_location('variant_generator', GEN)
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)
g.ROOT, g.KERNEL = ROOT, ROOT / 'kernel'
g.MIDASM_TOML = ROOT / 'tools/regen/superman_regen.toml'
SCRATCH = ROOT / 'archive/override_variant_tests'
SCRATCH.mkdir(parents=True, exist_ok=True)
SYM = 'sub_82100310'


def body(mutates=False, gate=''):
    return (f'PPC_FUNC({SYM})\n{{\n' +
            (f'  if (getenv("{gate}")) ctx.r3.u32 = 1;\n' if gate else '') +
            ('  ctx.r4.u32 = 2;\n' if mutates else '') +
            f'  __imp__{SYM}(ctx, base);\n}}\n')


@contextlib.contextmanager
def fixture(files):
    with tempfile.TemporaryDirectory(dir=SCRATCH) as directory:
        root = Path(directory)
        (root / 'kernel').mkdir()
        (root / 'guest').mkdir()
        (root / 'docs').mkdir()
        for name, text in files.items():
            (root / 'kernel' / name).write_text(text, encoding='utf-8')
        with mock.patch.multiple(g, ROOT=root, KERNEL=root / 'kernel',
                                 MANIFEST=root / 'docs/OVERRIDES.json',
                                 MIDASM_TOML=root / 'absent.toml'):
            yield root


def assert_both(row):
    assert row['gates_scope'] == 'all-source-variants'
    assert row['gates'] == ['SR_ORACLE_WRITE']
    assert row['auto'] == 'needs-review'
    assert [v['auto'] for v in row['variants']] == ['needs-review', 'observe']
    assert [v['conditions'] for v in row['variants']] == [
        [['#if SR_ORACLE']], [['#if SR_ORACLE', '#else']]]
    assert all(v['why'].startswith('imp_calls=1') for v in row['variants'])


class Variants(unittest.TestCase):
    def test_real_oracle_then_release_shape(self):
        with fixture({'both.cpp': '#if SR_ORACLE\n' + body(gate='SR_ORACLE_WRITE') + '#else\n' + body() + '#endif\n'}):
            assert_both(g.scan()[SYM])

    def test_reverse_order_retains_mutating_tail(self):
        with fixture({'both.cpp': '#if !SR_ORACLE\n' + body() + '#else\n' + body(gate='SR_ORACLE_WRITE') + '#endif\n'}):
            row = g.scan()[SYM]
            self.assertEqual([v['auto'] for v in row['variants']], ['observe', 'needs-review'])
            self.assertEqual(row['gates'], ['SR_ORACLE_WRITE'])
            self.assertEqual(row['auto'], 'needs-review')

    def test_nested_elif_and_disabled_branch_are_inventory(self):
        source = '#if OUTER\n#if 0\n' + body() + '#elif UNKNOWN\n' + body(True) + '#else\n' + body() + '#endif\n#endif\n'
        with fixture({'branches.cpp': source}):
            variants = g.scan()[SYM]['variants']
            self.assertEqual(len(variants), 3)
            self.assertEqual(variants[1]['conditions'], [['#if OUTER'], ['#if 0', '#elif UNKNOWN']])
            self.assertEqual(variants[2]['conditions'], [['#if OUTER'], ['#if 0', '#elif UNKNOWN', '#else']])

    def test_unconditional_duplicate_is_not_promoted(self):
        with fixture({'a.cpp': body(), 'b.cpp': body()}):
            row = g.scan()[SYM]
            self.assertEqual(row['auto'], 'needs-review')
            self.assertEqual([v['file'] for v in row['variants']], ['a.cpp', 'b.cpp'])
            with contextlib.redirect_stdout(io.StringIO()):
                g.main()
            saved = json.loads(g.MANIFEST.read_text())[SYM]
            self.assertEqual(saved['class'], 'needs-review')
            self.assertEqual(saved['variants'], row['variants'])

    def test_conditional_macro_definitions_are_kept(self):
        source = ('#if SR_ORACLE\n#define TEST_HOOK(sym) PPC_FUNC(sym) { ctx.r3.u32 = 1; __imp__##sym(ctx, base); }\n'
                  '#else\n#define TEST_HOOK(sym) PPC_FUNC(sym) { __imp__##sym(ctx, base); }\n#endif\n'
                  f'TEST_HOOK({SYM})\n')
        with fixture({'macro.cpp': source}):
            variants = g.scan()[SYM]['variants']
            self.assertEqual([v['auto'] for v in variants], ['needs-review', 'observe'])
            self.assertEqual([v['macro_conditions'] for v in variants], [
                [['#if SR_ORACLE']], [['#if SR_ORACLE', '#else']]])

    def test_fake_directives_and_multiline_conditions(self):
        source = ('/*\n#else\n*/\nconst char* s = R"(\n#endif\n)";\n'
                  '#if SR_ORACLE && \\\n FEATURE\n' + body() + '#else\n' + body() + '#endif\n')
        with fixture({'lexical.cpp': source}):
            row = g.scan()[SYM]
            self.assertEqual(len(row['variants']), 2)
            self.assertIn('FEATURE', row['variants'][0]['conditions'][0][0])

    def test_raw_embedded_quote_cannot_open_directive(self):
        source = ('const char* s = R"tag(text "\n#else\n)tag";\n'
                  '#if REAL\n' + body() + '#else\n' + body() + '#endif\n')
        with fixture({'raw.cpp': source}):
            variants = g.scan()[SYM]['variants']
            self.assertEqual(variants[0]['conditions'], [['#if REAL']])
            self.assertEqual(variants[0]['line'], 5)

    def test_directive_literals_and_comments_preserve_condition(self):
        source = ("#if MAGIC == 'x' && MORE == '\\'' /* ignore \" */\n" + body() +
                  '#elif VALUE == 1\'000 // ignored\n' + body() + '#endif\n')
        with fixture({'literal.cpp': source}):
            variants = g.scan()[SYM]['variants']
            self.assertEqual(variants[0]['conditions'], [["#if MAGIC == 'x' && MORE == '\\''"]])
            self.assertEqual(variants[1]['conditions'][0][-1], "#elif VALUE == 1'000")

    def test_splicing_happens_before_comment_and_directive_detection(self):
        source = ('// continued \\\n#if FAKE\n'
                  '/\\\n/ split delimiter \\\n#else\n'
                  'const int value = 1\'000;\n'
                  '#if RE\\\nAL\n' + body() + '#else\n' + body() + '#endif\n')
        with fixture({'splice.cpp': source}):
            variants = g.scan()[SYM]['variants']
            self.assertEqual(variants[0]['conditions'], [['#if REAL']])
            self.assertEqual(variants[0]['line'], 9)

    def test_malformed_directives_are_rejected(self):
        for source in ('#else\n', '#if REAL\n', '#if REAL\n#else\n#elif BAD\n#endif\n'):
            with self.subTest(source=source), self.assertRaises(ValueError):
                g.conditional_locations(source)

    def test_missing_branch_and_observer_promotion_controls(self):
        source = '#if SR_ORACLE\n' + body(gate='SR_ORACLE_WRITE') + '#else\n' + body() + '#endif\n'
        with fixture({'both.cpp': source}):
            good = g.scan()[SYM]
            for mutation in ('drop-first', 'promote', 'lose-gates'):
                bad = json.loads(json.dumps(good))
                if mutation == 'drop-first':
                    bad['variants'] = bad['variants'][1:]
                elif mutation == 'promote':
                    bad['auto'] = 'observe'
                else:
                    bad['gates'] = []
                with self.subTest(mutation=mutation), self.assertRaises(AssertionError):
                    assert_both(bad)

    def test_previous_last_definition_behavior_is_detected(self):
        source = '#if SR_ORACLE\n' + body(gate='SR_ORACLE_WRITE') + '#else\n' + body() + '#endif\n'
        with fixture({'both.cpp': source}), mock.patch.object(g, 'retain_variants', lambda found, variants: None):
            with self.assertRaises((AssertionError, KeyError)):
                assert_both(g.scan()[SYM])

    def test_audit_does_not_apply_one_variants_gate_to_another(self):
        sys.path.insert(0, str(ROOT / 'scripts'))
        import audit_forcing as af
        source = ('#if SR_ORACLE\n' + body(gate='SR_ORACLE_WRITE').replace(
            'ctx.r3.u32 = 1;', 'sr::kernel::writeGuestBE32(0x1000, 1u);') +
            '#else\n' + body(True).replace(
            'ctx.r4.u32 = 2;', 'sr::kernel::writeGuestBE32(0x2000, 2u);') + '#endif\n')
        with fixture({'both.cpp': source}) as root:
            row = g.scan()[SYM]
            self.assertEqual(row['gates'], ['SR_ORACLE_WRITE'])
            row['class'] = 'band-aid'
            for gates in ([], ['SR_ORACLE_WRITE'], ['SR_ALLOW_FORCING']):
                row['gates'] = gates
                g.MANIFEST.write_text(json.dumps({SYM: row}))
                self.assertEqual(af.load_band_aid_syms(str(root)), {SYM})
                _, sites = af.collect(str(root))
                ungated = [site for site in sites if site[0] == 'UNGATED']
                self.assertTrue(any('writeGuestBE32(0x2000, 2u)' in site[6] for site in ungated), sites)

    def test_single_definition_shape_is_unchanged(self):
        with fixture({'single.cpp': body()}):
            self.assertEqual(g.scan()[SYM], {
                'file': 'single.cpp', 'gates': [], 'auto': 'observe', 'why': 'imp_calls=1'})

    def test_current_registry_has_no_lost_symbols_gates_or_manual_fields(self):
        previous = json.loads((ROOT / 'docs/OVERRIDES.json').read_text())
        with tempfile.TemporaryDirectory(dir=SCRATCH) as directory:
            manifest = Path(directory) / 'OVERRIDES.json'
            manifest.write_text(json.dumps(previous))
            with mock.patch.object(g, 'MANIFEST', manifest), contextlib.redirect_stdout(io.StringIO()):
                g.main()
            current = json.loads(manifest.read_text())
        self.assertEqual(set(current), set(previous))
        for symbol, old in previous.items():
            with self.subTest(symbol=symbol):
                new = current[symbol]
                self.assertLessEqual(set(old['gates']), set(new['gates']))
                for field in ('class', 'evidence', 'retire', 'default_exe'):
                    self.assertEqual(new.get(field), old.get(field))
        for symbol, mechanical in g.scan().items():
            with self.subTest(symbol=symbol):
                self.assertEqual(current[symbol].get('variants'), mechanical.get('variants'))
                if 'variants' in mechanical:
                    self.assertEqual(current[symbol]['auto'], 'needs-review')
                    self.assertEqual(current[symbol]['gates_scope'], 'all-source-variants')


if __name__ == '__main__':
    unittest.main()
