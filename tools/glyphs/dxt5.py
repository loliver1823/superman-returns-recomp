"""Minimal DXT5 (BC3) decoder for the game's Apt icon payloads (8in16-swapped, linear blocks).

Used by tools/glyphs/glyph_map.py --extract and tools/glyphs/preview.py to show the game's own
icons next to their replacements. Not used by the game.
"""
import struct


def _rgb565(c):
    return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)


def decode_dxt5(data, w, h, swap=True):
    img = bytearray(w * h * 4)
    bx, by = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    p = 0
    for ty in range(by):
        for tx in range(bx):
            blk = data[p:p + 16]
            p += 16
            if len(blk) < 16:
                break
            if swap:
                blk = bytes(v for i in range(0, 16, 2) for v in (blk[i + 1], blk[i]))
            a0, a1 = blk[0], blk[1]
            ab = int.from_bytes(blk[2:8], "little")
            al = [a0, a1] + ([((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)] if a0 > a1
                             else [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255])
            c0, c1, cb = struct.unpack("<HHI", blk[8:16])
            cs = [_rgb565(c0), _rgb565(c1)]
            cs += [tuple((2 * cs[0][i] + cs[1][i]) // 3 for i in range(3)),
                   tuple((cs[0][i] + 2 * cs[1][i]) // 3 for i in range(3))]
            for py in range(4):
                for px in range(4):
                    x, y = tx * 4 + px, ty * 4 + py
                    if x < w and y < h:
                        o = (y * w + x) * 4
                        r, g, b = cs[(cb >> (2 * (py * 4 + px))) & 3]
                        img[o:o + 4] = bytes((r, g, b, al[(ab >> (3 * (py * 4 + px))) & 7]))
    return bytes(img)
