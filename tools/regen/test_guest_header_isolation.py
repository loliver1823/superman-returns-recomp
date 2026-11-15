"""Fail-closed coverage checks for the common-prefix dependency proof."""
from pathlib import Path
import tempfile
import unittest

from check_guest_header_isolation import ROOT, generated_prefix


class PrefixTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / 'archive/guest_header_census_tests'
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        (self.folder / 'ppc_recomp_shared.h').write_text(
            '#pragma once\n#include "ppc_config.h"\n#include "ppc_context.h"\n', encoding='utf-8')
        for index in (0, 10, 134):
            (self.folder / f'ppc_recomp.{index}.cpp').write_text(
                '#include "ppc_recomp_shared.h"\n// fixture body\n', encoding='utf-8')

    def test_every_numeric_generated_unit_is_counted(self):
        self.assertEqual(generated_prefix(self.folder), 3)

    def test_extra_host_include_in_any_unit_refuses_common_prefix_proof(self):
        with (self.folder / 'ppc_recomp.134.cpp').open('a', encoding='utf-8') as stream:
            stream.write('#include "kernel/guest_context.hpp"\n')
        with self.assertRaisesRegex(ValueError, 'include prefix changed'):
            generated_prefix(self.folder)

    def test_shared_header_cannot_reintroduce_host_dependency(self):
        with (self.folder / 'ppc_recomp_shared.h').open('a', encoding='utf-8') as stream:
            stream.write('#include "kernel/guest_context.hpp"\n')
        with self.assertRaisesRegex(ValueError, 'shared-header include prefix changed'):
            generated_prefix(self.folder)

    def test_empty_generated_tree_is_not_a_pass(self):
        for path in self.folder.glob('ppc_recomp.[0-9]*.cpp'):
            path.unlink()
        with self.assertRaisesRegex(ValueError, 'No generated guest units'):
            generated_prefix(self.folder)


if __name__ == '__main__':
    unittest.main()
