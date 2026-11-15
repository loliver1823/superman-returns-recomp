"""lane E L-c: the 9-row pause menu (custom_frontend 40_pause.patch + srpause.aptasm) on the player's actual read-only
ui.AST - the rows' positions, the re-purposed rows' labels, PHOTO MODE's slot hidden, A routed to SRPause.OnRow ahead
of the game's own branches (and nothing else after it), the two-choice QUIT (its popup's ordered arguments, and what
each answer does: QUIT TO TITLE is the retail quitCallBack's YES, read from the disc), no MORE OPTIONS left
(docs/CUSTOM_FRONTEND.md 5.14). The checks read the bodies of the functions they name, not the whole class block, and
the MUTANT tests (the L-c review's P2-1) build broken copies of the recipe in memory and require the checks to FAIL.

    python tools/apt/test_pause_patch.py        (needs the disc: bgfa.DEFAULT_UI, or SR_UI_AST=<ui.AST> from a
                                                 worktree without the disc junction)
"""
import os
import re
import shutil
import struct
import tempfile
import unittest
from pathlib import Path

from bgfa import DEFAULT_UI, Bgfa, TAG_APT, TAG_CONST
from apt_model import Apt
import patch_movie as pm
from rt_patch import view, frame_items

MOD = Path(__file__).parent / "mods/custom_frontend"
PATCH = "40_pause.patch"
RETAIL_Y = [58.0, 96.0, 134.0, 192.0, 230.0, 268.0, 306.0, 344.0, 402.0]   # btn1 .. btn9 (SCRPAUSEMENU char 2)
NINE_ROW_Y = [68.0, 106.0, 144.0, 182.0, 220.0, 258.0, 296.0, 334.0, 392.0]
TARGET = re.compile(r" ->(\d+)$")


def sub_index(seq, sub):
    """first index of the contiguous run `sub` in `seq`, or -1"""
    for i in range(len(seq) - len(sub) + 1):
        if seq[i:i + len(sub)] == sub:
            return i
    return -1


def target(ins):
    m = TARGET.search(ins)
    assert m, "not a branch: " + ins
    return int(m.group(1))


def rebase(seq, base):
    """branch targets relative to `base` (two copies of the same code compare equal wherever they sit)"""
    return [TARGET.sub(lambda m: " ->@%d" % (int(m.group(1)) - base), x) for x in seq]


class Movie:
    """SCRPAUSEMENU built from a recipe directory (None = the disc's own movie) and the view of its class block"""

    def __init__(self, apt, cst, mod_dir):
        self.p = pm.Patcher(apt, cst, "SCRPAUSEMENU")
        if mod_dir is not None:
            name, ops = pm.load_patch(str(Path(mod_dir) / PATCH))[0]
            assert name == "SCRPAUSEMENU", name
            for op in ops:
                self.p.apply(op)
        self.apt, self.cst, self.checks = self.p.finish()
        self.a = Apt(self.apt, self.cst)
        key, _ = pm.find_function(pm.Ctx(self.p.g), "InitGui", "ScrPauseMenu", None, "test")
        self.v = view(self.a, self.p.where[key])

    def fn(self, name):
        """(lo, hi): the instruction range of the ONE function defined as <obj>.<name> in the block"""
        hits = [i for i in range(len(self.v) - 1)
                if self.v[i] == "EA_PushConstByte? '%s'" % name and self.v[i + 1].startswith("DefineFunction2")]
        assert len(hits) == 1, (name, hits)
        d = hits[0] + 1
        return d + 1, int(self.v[d].rsplit("end->", 1)[1])

    def body(self, name):
        lo, hi = self.fn(name)
        return self.v[lo:hi]


# ---------------------------------------------------------------- the checks (each raises AssertionError)
def check_routing(m, retail):
    """HandleInput: the prepend at its head sends A on btn4/5/6/7/9 (and only those) to SRPause.OnRow, then returns
    false; everything after the prepend is the game's own HandleInput, instruction for instruction"""
    lo, hi = m.fn("HandleInput")
    v = m.v
    head = ["Push [reg4]", "Push [reg2]", "EA_GetNamedMember 'INPUTCODE'", "EA_GetNamedMember 'CROSS'", "Equals2", "Not"]
    assert v[lo:lo + 6] == head and v[lo + 6].startswith("If ->"), v[lo:lo + 7]
    done = target(v[lo + 6])
    i = lo + 7 + 10   # getSelectedItem -> reg5, its _name -> reg3
    assert v[lo + 7:i] == ["op_59", "Push [reg2]", "EA_GetNamedMember 'gFM'", "EA_PushConstByte? 'getSelectedItem'",
                           "CallMethod", "StoreRegister 5", "Pop", "Push [reg5]", "EA_GetNamedMember '_name'",
                           "StoreRegister 3"], v[lo + 7:i]
    assert v[i] == "Pop"
    i += 1
    rows, mine = [], None
    while v[i] == "Push [reg3]":
        assert v[i + 2] == "Equals2" and v[i + 3].startswith("If ->"), v[i:i + 4]
        rows.append(v[i + 1].split("'")[1])
        mine = target(v[i + 3]) if mine is None else mine
        assert target(v[i + 3]) == mine, "every row goes to the same action"
        i += 4
    assert rows == ["btn4", "btn5", "btn6", "btn7", "btn9"], rows
    assert v[i].startswith("Jump ->") and target(v[i]) == done and i + 1 == mine, v[i]
    action = ["Push [reg2]", "EA_PushConstByte? 'PauseSelectedItem'", "Push [reg3]", "SetMember",
              "Push [reg4]", "op_5A", "Push [reg5]", "EA_CallNamedMethodPop 'handleInput'",
              "Push [reg2]", "EA_GetNamedMember 'SOUND'", "EA_GetNamedMember 'OPEN'", "op_5A", "Push [reg2]",
              "EA_CallNamedMethodPop 'playSound'",
              "Push [reg3]", "op_5A", "Push [reg2]", "EA_GetNamedMember 'SRPause'", "EA_CallNamedMethodPop 'OnRow'",
              "op_74", "Return"]
    assert v[mine:mine + len(action)] == action, v[mine:mine + len(action)]
    assert mine + len(action) == done, "the Return is the action's last instruction: no fall-through into the game's code"
    rlo, rhi = retail.fn("HandleInput")
    assert rebase(v[done:hi], done) == rebase(retail.v[rlo:rhi], rlo), "the game's own HandleInput, unchanged, after it"


def retail_quit_yes(retail):
    """the retail quitCallBack's YES (button 1, mCurAction 0): the CLOSE sound, QuitGame, ChangeInterfaceState("IG"),
    PauseSelectedItem = btn1 - read from the disc's own function"""
    b = retail.body("quitCallBack")
    snd = sub_index(b, ["Push [reg2]", "EA_GetNamedMember 'SOUND'", "EA_GetNamedMember 'CLOSE'", "op_5A", "Push [reg2]",
                        "EA_CallNamedMethodPop 'playSound'"])
    q = sub_index(b, ["op_59", "Push [reg2]", "EA_GetNamedMember 'StateAdapter'", "EA_CallNamedMethodPop 'QuitGame'"])
    ig = b.index("EA_PushConstByte? 'IG'")
    tail = b[ig:ig + 9]
    assert snd >= 0 and q > snd and ig > q, (snd, q, ig)
    assert tail[-1] == "SetMember" and "EA_PushConstByte? 'PauseSelectedItem'" in tail and \
        "EA_CallNamedMethodPop 'ChangeInterfaceState'" in tail, tail
    return b[snd:snd + 6] + b[q:q + 4] + tail


def check_quit(m, retail):
    """ShowQuit's popup: ShowGenPop(WARNING, AREYOUSUREQUITGAME, 2 buttons, default 0, [QUIT TO TITLE,
    QUIT TO DESKTOP], OnQuit, OnReady, SRPause, B cancels, closeOnPress, 29153); OnQuit: < 0 = Defer(Back, 9),
    1 = SR_Quit, 0 = exactly the retail YES"""
    s = m.body("ShowQuit")
    args = ["EA_PushShort 29153", "op_73", "op_73", "Push [reg1]", "Push [reg1]", "EA_GetNamedMember 'OnReady'",
            "Push [reg1]", "EA_GetNamedMember 'OnQuit'",
            "EA_PushConstByte? 'QUIT TO DESKTOP'", "EA_PushConstByte? 'QUIT TO TITLE'",   # InitArray: the last
            "EA_PushByte 2", "InitArray",                                                       # pushed is element 0
            "op_59", "EA_PushByte 2",
            "EA_PushConstByte? 'Generic_AREYOUSUREQUITGAME'", "op_5A", "Push [reg3]",
            "EA_PushConstByte? 'GetLocalizedText'", "CallMethod",
            "EA_PushConstByte? 'Generic_WARNING'", "op_5A", "Push [reg3]", "EA_PushConstByte? 'GetLocalizedText'",
            "CallMethod",
            "EA_PushByte 11", "Push [reg2]", "EA_GetNamedMember 'FlowMgr'", "EA_CallNamedMethodPop 'ShowGenPop'"]
    assert s[-len(args):] == args, s[-len(args):]
    lo, hi = m.fn("OnQuit")
    v = m.v
    i = v.index("Less2", lo)
    assert v[i - 2:i] == ["Push [reg4]", "op_59"] and v[i + 1] == "Not" and v[i + 2].startswith("If ->"), v[i - 2:i + 3]
    x1 = target(v[i + 2])
    assert v[i + 3:x1] == ["EA_PushByte 9", "EA_PushConstByte? 'Back'", "EA_PushByte 2", "Push [reg1]",
                           "EA_CallNamedMethodPop 'Defer'", "op_76", "Return"], ("cancel", v[i + 3:x1])
    assert v[x1:x1 + 4] == ["Push [reg4]", "op_5A", "Equals2", "Not"] and v[x1 + 4].startswith("If ->"), v[x1:x1 + 5]
    x2 = target(v[x1 + 4])
    assert v[x1 + 5:x2] == ["GetURL [b'FSCommand:SR_Quit', b'']", "op_76", "Return"], ("desktop", v[x1 + 5:x2])
    assert v[x2:hi] == retail_quit_yes(retail), ("main menu: the retail YES, exactly", v[x2:hi])


def check_labels(m):
    init = m.body("InitGui")
    for label, btn in (("EA_PushConstByte? 'ACHIEVEMENTS'", "btn4"), ("EA_PushConstByte? 'OPTIONS'", "btn7")):
        assert sub_index(init, [label, "op_5A", "Push [reg1]", "EA_GetNamedMember '%s'" % btn,
                                "EA_CallNamedMethodPop 'SetText'"]) >= 0, btn
    for key, btn in (("Label_ScrStatistics", "btn5"), ("Label_ScrMovesList", "btn6")):   # the game's own text
        assert sub_index(init, ["EA_PushConstByte? '%s'" % key, "op_5A", "Push [reg2]",
                                "EA_GetNamedMember 'LocalizationAdapter'", "EA_PushConstByte? 'GetLocalizedText'",
                                "CallMethod", "op_5A", "Push [reg1]", "EA_GetNamedMember '%s'" % btn,
                                "EA_CallNamedMethodPop 'SetText'"]) >= 0, btn
    assert sub_index(init, ["op_59", "Push [reg1]", "EA_GetNamedMember 'btn8'",
                            "EA_CallNamedMethodPop 'disable'"]) >= 0, "btn8 out of the focus order"
    assert sub_index(init, ["Push [reg1]", "EA_GetNamedMember 'btn8'", "EA_PushConstByte? '_visible'", "op_74",
                            "SetMember"]) >= 0, "btn8 hidden"


class PausePatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(os.environ.get("SR_UI_AST") or DEFAULT_UI, "rb") as source:
            ui = Bgfa(source.read())
        # A missing disc or movie is a failure, never a skipped success.
        entry = next(x for x in ui.entries if x["name"] == "SCRPAUSEMENU")
        movie = Bgfa(entry["data"])
        cls.apt = next(x["data"] for x in movie.entries if x["tag"] == TAG_APT)
        cls.cst = next(x["data"] for x in movie.entries if x["tag"] == TAG_CONST)
        cls.retail = Movie(cls.apt, cls.cst, None)
        cls.m = Movie(cls.apt, cls.cst, MOD)

    @staticmethod
    def places(a):
        """{PlaceObject name: ty} of SCRPAUSEMENU's button sprite (char 2), frame 0"""
        off, _ = a.chars[2]
        out = {}
        for t, it in frame_items(a, off, 0):
            if t != 3:
                continue
            nm = a.u(it + 0x34)
            if nm:
                out[a.b[nm:a.b.index(b"\0", nm)].decode("latin1")] = struct.unpack_from(">f", a.b, it + 0x24)[0]
        return out

    def mutant(self, rel, old, new, check):
        """the recipe with one textual change, built in memory (a temporary copy of the mod directory); `check` passes
        on the recipe as it is (so its failure on the mutant is the mutation's, never a check that cannot pass)"""
        check(self.m, self.retail)
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp) / "custom_frontend"
            shutil.copytree(MOD, d)
            f = d / rel
            text = f.read_text(encoding="utf-8")
            self.assertEqual(text.count(old), 1, "the mutation applies exactly once: " + old)
            f.write_bytes(text.replace(old, new).encode("utf-8"))
            m = Movie(self.apt, self.cst, d)
        with self.assertRaises(AssertionError):
            check(m, self.retail)

    # ---- the recipe as it is
    def test_rows_on_one_pitch_quit_spacer_kept_no_clone(self):
        y0, y = self.places(self.retail.a), self.places(self.m.a)
        self.assertEqual([y0["btn%d" % i] for i in range(1, 10)], RETAIL_Y, "the retail layout this patch starts from")
        self.assertEqual([y["btn%d" % i] for i in range(1, 10)], NINE_ROW_Y)
        self.assertEqual(sorted(y), sorted(y0), "the same nine buttons: no clone (MORE OPTIONS' btn10 is gone)")
        self.assertTrue(self.m.checks)

    def test_repurposed_labels_and_the_hidden_photo_mode_slot(self):
        check_labels(self.m)

    def test_a_on_a_repurposed_row_goes_to_onrow_then_returns_ahead_of_the_games_own_code(self):
        check_routing(self.m, self.retail)

    def test_two_choice_quit_main_menu_is_the_retail_yes(self):
        check_quit(self.m, self.retail)

    def test_no_more_options(self):
        blob = self.m.apt + self.m.cst   # the movie and its constants (the strings live in the .const pool)
        for s in (b"QUIT TO TITLE\0", b"QUIT TO DESKTOP\0", b"FSCommand:SR_Quit\0", b"FSCommand:SR_Achievements\0"):
            self.assertTrue(s in blob, s)
        for s in (b"MORE OPTIONS\0", b"ShowPanel\0", b"OnPanel\0", b"btn10\0"):
            self.assertFalse(s in blob, s)

    # ---- mutants: each breaks one behaviour; the check for it must fail (the review's P2-1)
    def test_mutant_no_return_after_onrow(self):
        self.mutant(PATCH, "    EA_CallNamedMethodPop 'OnRow'\n    op_74\n    Return\n",
                        "    EA_CallNamedMethodPop 'OnRow'\n", check_routing)

    def test_mutant_btn8_routed(self):
        self.mutant(PATCH, "    EA_PushConstByte 'btn9'\n", "    EA_PushConstByte 'btn8'\n", check_routing)

    def test_mutant_quitgame_misnamed(self):
        self.mutant("srpause.aptasm", "        EA_CallNamedMethodPop 'QuitGame'\n",
                        "        EA_CallNamedMethodPop 'QuitGameNowhere'\n", check_quit)

    def test_mutant_quit_labels_reversed(self):
        self.mutant("srpause.aptasm",
                        "        EA_PushConstByte 'QUIT TO DESKTOP'\n        EA_PushConstByte 'QUIT TO TITLE'\n",
                        "        EA_PushConstByte 'QUIT TO TITLE'\n        EA_PushConstByte 'QUIT TO DESKTOP'\n", check_quit)

    def test_mutant_quit_without_the_interface_change(self):
        # the r0 recipe (the review's P1-1): QuitGame alone
        self.mutant("srpause.aptasm",
                        "        EA_PushConstByte 'IG'\n        EA_PushOne\n        Push [reg2]\n"
                        "        EA_GetNamedMember 'FlowMgr'\n        EA_CallNamedMethodPop 'ChangeInterfaceState'\n"
                        "        Push [reg2]\n        EA_PushConstByte 'PauseSelectedItem'\n"
                        "        EA_PushConstByte 'btn1'\n        SetMember\n    .end", "    .end", check_quit)


if __name__ == "__main__":
    unittest.main()
