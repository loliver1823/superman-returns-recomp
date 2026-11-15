"""Native costume-screen splice checks on the player's actual read-only ui.AST."""
import unittest
from pathlib import Path

from bgfa import DEFAULT_UI, Bgfa, TAG_APT, TAG_CONST
from apt_model import Apt
import patch_movie as pm
from rt_patch import view, expect


class CostumePatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(DEFAULT_UI, "rb") as source:
            cls.ui = Bgfa(source.read())
        # A missing disc or movie is a failure, never a skipped success.
        entry = next(x for x in cls.ui.entries if x["name"] == "SCRCOSTUMES")
        cls.movie = Bgfa(entry["data"])
        cls.apt = next(x["data"] for x in cls.movie.entries if x["tag"] == TAG_APT)
        cls.cst = next(x["data"] for x in cls.movie.entries if x["tag"] == TAG_CONST)

    def patch(self, options="after-call=GetCostumeData", snippet="EA_PushZero\nPop"):
        _, ops = pm.parse_patch("movie SCRCOSTUMES\nappend-to function InitGui in ScrCostumes " +
                                options + "\n" + snippet + "\nend\n")[0]
        p = pm.Patcher(self.apt, self.cst, "SCRCOSTUMES")
        result = p.apply(ops[0])
        return p, result

    def test_insert_after_real_getter_without_changing_retail_flow(self):
        p, result = self.patch()
        original = view(Apt(self.apt, self.cst), result["code"])
        getter = original.index("EA_PushConstByte? 'GetCostumeData'")
        self.assertEqual(original[getter + 1], "CallMethod")
        self.assertEqual(result["at"], getter + 2)
        apt, cst, checks = p.finish()
        actual = view(Apt(apt, cst), p.where[result["code"]])
        self.assertEqual(actual, expect(original, getter + 2, ["op_59", "Pop"]))
        self.assertTrue(checks)

    def test_missing_call_is_rejected(self):
        with self.assertRaises(pm.PatchError):
            self.patch("after-call=NoSuchMethod")

    def test_duplicate_call_is_rejected(self):
        p, _ = self.patch(snippet="EA_PushZero\nPush [reg2]\nEA_PushConstByte 'GetCostumeData'\nCallMethod\nPop")
        _, ops = pm.parse_patch("movie SCRCOSTUMES\nappend-to function InitGui in ScrCostumes "
                                "after-call=GetCostumeData\nEA_PushZero\nPop\nend")[0]
        with self.assertRaises(pm.PatchError):
            p.apply(ops[0])

    def test_shipped_picker_extends_getter_and_keeps_retail_selection(self):
        patch = Path(__file__).parent / "mods/custom_frontend/50_costumes.patch"
        _, ops = pm.load_patch(str(patch))[0]
        p = pm.Patcher(self.apt, self.cst, "SCRCOSTUMES")
        for op in ops:
            p.apply(op)
        apt, cst, _ = p.finish()
        # Decode the emitted bytes independently of the assembler/patch graph.
        ctx = pm.Ctx(p.g)
        key, _ = pm.find_function(ctx, "InitGui", "ScrCostumes", None, "test")
        code = view(Apt(apt, cst), p.where[key])
        getter = code.index("EA_PushConstByte? 'GetCostumeData'")
        self.assertEqual(code[getter + 1:getter + 8], [
            "CallMethod", "op_5A", "Push [reg2]", "EA_GetNamedMember 'SRCostumes'",
            "EA_PushConstByte? 'Append'", "CallMethod", "StoreRegister 4"])
        self.assertEqual(code.count("EA_CallNamedMethodPop 'SetActiveCostume'"), 1)
        self.assertIn("EA_GetNamedMember 'SRId'", code)
        self.assertIn("EA_CallNamedMethodPop 'Wear'", code)
        # The helper is destroyed before the native screen unloads its movie.
        exit_key, exit_fn = pm.find_function(ctx, "ExitScreen", "ScrCostumes", None, "test")
        graph_code = p.g.blocks[exit_key].code
        self.assertEqual([i.op for i in graph_code[exit_fn + 1:exit_fn + 5]], [0x96, 0xA2, 0x3A, 0x17])


if __name__ == "__main__":
    unittest.main()
