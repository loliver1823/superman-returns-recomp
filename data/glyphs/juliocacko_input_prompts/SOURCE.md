# Julio Cacko - FREE Input Prompts Pack (subset)

- **Author:** Julio Cacko
- **Pack:** "FREE Input Prompts Pack", Universal Version
- **Where it comes from:** https://juliocacko.gumroad.com/l/free-input-pack (the pack's own
  `credit.txt`) and https://juliocacko.itch.io/free-input-prompts
- **Licence:** the art is **CC0 1.0 Universal** (public domain dedication; full text in
  `LICENSE.txt` next to this file). The itch.io page says: "Free to use in any project,
  commercial or non-commercial. No attribution required (but appreciated!)". The pack's code
  is MIT; no code from it is used here. We credit the author anyway (`CREDITS.md`).
- **Source archive:** `Universal Version.zip`, SHA-256
  `f6c75921b9d6ab2cb2b5249f547c7b7ba19e22da9f4f3b45693cf0912f25ded0` (1633 entries; 1580 PNG,
  21 SVG under `Source/{Keyboard_Mouse,XGamepad,P4Gamepad,P5Gamepad,SGamepad}`). The archive
  itself is not in the repository.

## What is here, and why

Only the files the port shows, plus every keyboard key so a rebind always has art. The PNGs
are unmodified copies from the archive (same file names); the game composes them into its
icon frames at run time (`gfx/glyph_subst.cpp`, recipes in `../glyph_map.txt`).

| Folder | Archive folder | Files | Used for |
|---|---|---|---|
| `P4Gamepad/` | `Source/P4Gamepad/Default/` | Cross/Circle/Square/Triangle `_Color`, L1, R1, L2, R2, Options, Share, Touch_Pad, Dpad, L, R, L3, R3 | PlayStation 4 (and 3) pads |
| `P5Gamepad/` | `Source/P5Gamepad/Default/` | the same names with `T_P5_` | DualSense |
| `SGamepad/` | `Source/SGamepad/Default/` | A, B, X, Y, LB, RB, LT, RT, Plus, Minus, Dpad, L, R, Home | Switch Pro / Joy-Con pair |
| `Keyboard_Mouse/White/` | `Source/Keyboard_Mouse/White/` | every key and mouse glyph (sprite sheet left out) | keyboard prompts: the key bound to each control (`settings/input.ini`) |
| `Keyboard_Mouse/Blanks/` | `Source/Keyboard_Mouse/Blanks/` | `T_Blank_White_Normal.png` | a bound key with no glyph of its own |

Xbox and generic pads keep the game's own Xbox 360 prompt art (it matches the rest of the
game's UI; docs/INPUT_GLYPHS.md "Xbox: the game's own icons"), so `XGamepad` is not shipped.
The `_Color` face buttons were chosen over the white ones because the game's own face-button
prompts are coloured; the white keyboard keys because the game's LB/RB/LT/RT/START/BACK
prompts are white.
