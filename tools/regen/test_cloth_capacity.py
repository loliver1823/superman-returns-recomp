import unittest
from pathlib import Path

import cloth_capacity as C


class GenerationTests(unittest.TestCase):
    def test_real_body_only_changes_reviewed_scratch_references(self):
        source = Path(__file__).resolve().parents[2] / "guest/ppc/ppc_recomp.35.cpp"
        generated = C.generate(source.read_text(encoding="utf-8"))
        self.assertEqual(generated.count("layout.tangent1"), 8)
        self.assertEqual(generated.count("layout.tangent2"), 7)
        self.assertEqual(generated.count("layout.frame + 20"), 5)
        self.assertEqual(generated.count("layout.accumulationBytes"), 2)

    def test_changed_arithmetic_refused_before_generation(self):
        source = Path(__file__).resolve().parents[2] / "guest/ppc/ppc_recomp.35.cpp"
        body = source.read_text(encoding="utf-8")
        body = body.replace("ea = -12112 + ctx.r1.u32;", "ea = -12128 + ctx.r1.u32;")
        with self.assertRaisesRegex(ValueError, "body changed"):
            C.generate(body)


if __name__ == "__main__": unittest.main()
