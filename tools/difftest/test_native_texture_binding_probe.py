"""Regression checks for source-byte fidelity and real probe input witnesses."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import native_texture_binding_probe as probe


class ProbeTests(unittest.TestCase):
    def test_crlf_unicode_changes_only_declared_rotation(self):
        original = ('// cape \u2014 diagnostic \u2713\r\n' + probe.ANCHOR +
                    '\r\nconst char* label = "\u00e9";\r\n').encode('utf-8')
        text, expected = probe.source_rotation(original)
        inserted = probe.ROTATION.replace('\n', '\r\n').encode('utf-8')
        self.assertEqual(expected.replace(inserted, probe.ANCHOR.encode(), 1), original)
        self.assertIn('\u2014', text)
        self.assertEqual(expected.count(b'\r\n'), expected.count(b'\n'))

    def test_lf_unicode_preserved(self):
        original = ('// \u2713\n' + probe.ANCHOR + '\n').encode('utf-8')
        _, mutated = probe.source_rotation(original)
        self.assertEqual(mutated.replace(probe.ROTATION.encode(), probe.ANCHOR.encode(), 1), original)

    def test_invalid_utf8_refused(self):
        with self.assertRaises(UnicodeDecodeError):
            probe.source_rotation(b'\xff' + probe.ANCHOR.encode())

    def test_missing_anchor_refused(self):
        with self.assertRaises(ValueError):
            probe.source_rotation(b'no binding here\n')

    def test_duplicate_anchor_refused(self):
        with self.assertRaises(ValueError):
            probe.source_rotation((probe.ANCHOR + '\n' + probe.ANCHOR).encode())

    def test_mixed_newlines_refused(self):
        with self.assertRaises(RuntimeError):
            probe.source_rotation(('// mixed\n' + probe.ANCHOR + '\r\n').encode())

    def test_declared_inputs_pin_objects_libraries_resources(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name in ('game.obj', 'icon.res', 'a library.lib'):
                (root / name).write_bytes(name.encode())
            ninja = ('build SupermanReturns.exe: LINK game.obj icon.res\n'
                     '  LINK_LIBRARIES = "a library.lib" -lkernel32\n')
            pins, implicit = probe.declared_link_inputs(ninja, root)
            self.assertEqual(set(pins), {str((root / p).resolve()) for p in
                                       ('game.obj', 'icon.res', 'a library.lib')})
            self.assertEqual(implicit, ['-lkernel32'])

    def test_missing_declared_object_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(RuntimeError):
                probe.declared_link_inputs('build SupermanReturns.exe: LINK missing.obj\n'
                                           '  LINK_LIBRARIES = missing.lib\n', Path(temp))

    def test_process_witness_is_written_with_supplied_environment(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            exe = folder / 'game.exe'; exe.write_bytes(b'fixture')
            path = folder / 'provenance.json'
            env = {'SR_MODS': '0', 'sr_lowercase': '1', 'SECRET': 'not-for-log'}
            terminal = {'pid': 123, 'exit_code': 0, 'terminal_observed': True, 'timed_out': False}
            with patch.object(probe.controls, 'run', return_value=terminal) as run:
                result, gates, pin = probe.run_witness(exe, folder, env, folder,
                                                     {'ProvenanceJson': str(path)}, {'proc': None})
            self.assertEqual(run.call_args.args[2], env)
            self.assertEqual(result, terminal)
            self.assertEqual(gates, {'SR_MODS': '0', 'sr_lowercase': '1'})
            saved = json.loads(path.read_text(encoding='utf-8'))
            self.assertEqual(saved['pid'], 123)
            self.assertTrue(saved['terminal_observed'])
            self.assertEqual(pin, probe.shared.digest(path))
            self.assertNotIn('not-for-log', path.read_text(encoding='utf-8'))

    def test_failed_spawn_leaves_explicit_nonterminal_witness(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            exe = folder / 'game.exe'; exe.write_bytes(b'fixture')
            path = folder / 'provenance.json'
            with patch.object(probe.controls, 'run', side_effect=OSError('spawn failed')):
                with self.assertRaises(OSError):
                    probe.run_witness(exe, folder, {}, folder,
                                      {'ProvenanceJson': str(path)}, {'proc': None})
            saved = json.loads(path.read_text(encoding='utf-8'))
            self.assertFalse(saved['terminal_observed'])
            self.assertIn('spawn failed', saved['error'])


if __name__ == '__main__':
    unittest.main()
