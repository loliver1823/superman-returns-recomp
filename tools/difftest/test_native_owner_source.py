"""Source invariants beside the CPU geometry and real HLSL compilation tests."""
from pathlib import Path
import importlib.util
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("gen_xesl_embed", ROOT / "tools/gen_xesl_embed.py")
embed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(embed)


def body(source, name):
    match = re.search(r"\b" + re.escape(name) + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise AssertionError("missing body " + name)
    start, depth = match.end(), 1
    for end in range(start, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end]
    raise AssertionError("unterminated body " + name)


def tokens(source):
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
    return re.findall(r"\w+|[^\s]", source)


# Pinned pre-cut NatOwnerPixel math; only its pitch expression changes.
ALIAS_BODY = """
  uint sx = uint(p.x) << (r & 15u);
  uint sy = uint(p.y) << ((r >> 4) & 15u);
  {
    uint tileW = (80u * max(gNatScaleX, 1u)) >> ri.edram_format_ints_log2, tileH = 16u * max(gNatScaleY, 1u);
    uint pitch = gNatAliasPitch;
    uint lin = (sy / tileH) * pitch + sx / tileW;
    [branch] if (lin >= 2048u && pitch != 0u) {
      lin -= (lin / 2048u) * 2048u;
      sx = (lin % pitch) * tileW + sx % tileW;
      sy = (lin / pitch) * tileH + sy % tileH;
    }
  }
  [branch] if ((r & 0x20000u) != 0u) {
    uint tileW = (80u * max(gNatScaleX, 1u)) >> ri.edram_format_ints_log2, half = tileW >> 1u;
    uint inTile = sx % tileW;
    sx += inTile >= half ? uint(-int(half)) : half;
  }
  return int2(int(sx >> ((r >> 8) & 15u)), int(sy >> ((r >> 12) & 15u)));
"""


class NativeOwnerSource(unittest.TestCase):
    def setUp(self):
        self.resolve = (ROOT / "gfx/native_resolve.inc").read_text(encoding="utf-8")
        self.upstream = (Path(embed.SRC) / "gpu/shaders/resolve.xesli").read_text(encoding="utf-8")

    def test_alias_math_unchanged(self):
        self.assertEqual(tokens(body(self.resolve, "NatAliasPixel")), tokens(ALIAS_BODY))
        self.assertIn("p = NatAliasPixel(ri, p, remap);", body(self.resolve, "NatRaw"))
        shmem = (ROOT / "gfx/d3d12_pm4_shmem.cpp").read_text(encoding="utf-8")
        targets = (ROOT / "gfx/native_targets.inc").read_text(encoding="utf-8")
        self.assertIn("depthC[11] = ri.srcKey.pitchTiles();", shmem)
        self.assertIn("depthC[11] = s.pitchTiles();", targets)

    def test_alias_source_mutants_detected(self):
        actual = body(self.resolve, "NatAliasPixel")
        for before, after in (("lin >= 2048u", "lin > 2048u"),
                              ("gNatAliasPitch", "gNatAliasPitch >> 16"),
                              ("0x20000u", "0u"), ("r >> 12", "r >> 8")):
            self.assertIn(before, actual)
            self.assertNotEqual(tokens(actual.replace(before, after)), tokens(ALIAS_BODY))

    def test_owner_selector_has_only_rectangles(self):
        select = body(self.resolve, "NatOwnerSel")
        for removed in ("2047", "2048", "pitch", "gNatBasePitch", "gNatRanges", "gNatMsaaRemap"):
            self.assertNotIn(removed, select)
        self.assertIn("gNatRects[i]", select)
        self.assertIn("p < int2(rect.zw)", select)

    def test_trimmed_closure_and_conversion_bodies(self):
        trimmed = embed.native_resolve_text(self.upstream)
        self.assertNotIn("gpu/shaders/edram.xesli", embed.FILES)
        self.assertNotIn("gpu/shaders/resolve.xesli", embed.FILES)
        for removed in ("xe_resolve_edram,", "XeEdramOffsetBytes", "XeResolveLoad",
                        "XE_RESOLVE_COPY_EDRAM_", '#include "edram.xesli"'):
            self.assertNotIn(removed, trimmed)
        # All remaining function bodies are byte-for-byte upstream code. This
        # includes destination addressing, format packing and sample conversion.
        names = re.findall(r"\b(Xe\w+)\([^;{}]*?\)\s*\{", trimmed)
        self.assertEqual(len(names), 14)
        self.assertIn("XeResolveDestPixelAddress", names)
        self.assertIn("XeResolveUnpack64bpp4Samples", names)
        for name in names:
            self.assertEqual(body(trimmed, name), body(self.upstream, name), name)
        generated = (ROOT / "third_party/xenia-shaders/xesl_embed.inc").read_text(encoding="utf-8")
        self.assertIn('R"XESL(' + trimmed + ')XESL"', generated)
        for source in embed.FILES:
            if source.endswith("/sr_resolve.xesli"):
                continue
            original = (Path(embed.SRC) / source).read_text(encoding="utf-8")
            self.assertIn('R"XESL(' + original + ')XESL"', generated)

    def test_trim_refuses_changed_anchors_and_restored_access(self):
        for before, after in (('#include "edram.xesli"', '#include "edram2.xesli"'),
                              ("uint XeResolveEdramPixelStrideBytes(", "uint ChangedStride(")):
            with self.assertRaises(ValueError):
                embed.native_resolve_text(self.upstream.replace(before, after))
        with self.assertRaises(ValueError):
            embed.native_resolve_text(self.upstream + "\nuint XeResolveLoadRestored;\n")


if __name__ == "__main__":
    unittest.main()
