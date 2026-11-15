#!/usr/bin/env python3
"""gen_mosaic.py — src/include/boxos_mosaic.h, Unicode's block mosaics as the
patterns they fill, written out of the character names the Python running it
knows (unicodedata; Unicode 16 or later, for the octants).

    python3 tools/gen_mosaic.py > src/include/boxos_mosaic.h

Not a build dependency: the header is checked in, and this is run again only
for a later Unicode.

Every shape is first a set of rectangles of the unit cell, read off its name:
a sextant or an octant by the sub-cells its name numbers, and the blocks
that complete their grids — halves, quarters, quadrants, the middle and
corner quarters — by what their names say they fill. A character is a
pattern of a grid, two across and two, three or four down, when its
rectangles cover each sub-cell whole or not at all; each grid must come out
whole, every pattern some character and none two, or nothing is written.
"""
import sys
import unicodedata

if tuple(int(p) for p in unicodedata.unidata_version.split(".")[:2]) < (16, 0):
    sys.exit("gen_mosaic: Unicode %s has no octants; 16.0 or later is needed" % unicodedata.unidata_version)

GRIDS = ((2, "Quadrants", "QUADRANTS"), (3, "Sextants", "SEXTANTS"), (4, "Octants", "OCTANTS"))

# The blocks that complete the mosaics, as their names describe them:
# (x0, y0, x1, y1) of the unit cell, y down. Each name is checked.
BLOCKS = {
    0x0020: ("SPACE", []),
    0x2588: ("FULL BLOCK", [(0, 0, 1, 1)]),
    0x2580: ("UPPER HALF BLOCK", [(0, 0, 1, 0.5)]),
    0x2584: ("LOWER HALF BLOCK", [(0, 0.5, 1, 1)]),
    0x258C: ("LEFT HALF BLOCK", [(0, 0, 0.5, 1)]),
    0x2590: ("RIGHT HALF BLOCK", [(0.5, 0, 1, 1)]),
    0x2582: ("LOWER ONE QUARTER BLOCK", [(0, 0.75, 1, 1)]),
    0x2586: ("LOWER THREE QUARTERS BLOCK", [(0, 0.25, 1, 1)]),
    0x1FB82: ("UPPER ONE QUARTER BLOCK", [(0, 0, 1, 0.25)]),
    0x1FB85: ("UPPER THREE QUARTERS BLOCK", [(0, 0, 1, 0.75)]),
    0x1FBE6: ("MIDDLE LEFT ONE QUARTER BLOCK", [(0, 0.25, 0.5, 0.75)]),
    0x1FBE7: ("MIDDLE RIGHT ONE QUARTER BLOCK", [(0.5, 0.25, 1, 0.75)]),
    0x1CEA8: ("LEFT HALF UPPER ONE QUARTER BLOCK", [(0, 0, 0.5, 0.25)]),
    0x1CEAB: ("RIGHT HALF UPPER ONE QUARTER BLOCK", [(0.5, 0, 1, 0.25)]),
    0x1CEA3: ("LEFT HALF LOWER ONE QUARTER BLOCK", [(0, 0.75, 0.5, 1)]),
    0x1CEA0: ("RIGHT HALF LOWER ONE QUARTER BLOCK", [(0.5, 0.75, 1, 1)]),
}
QUADRANT = {"UPPER LEFT": (0, 0, 0.5, 0.5), "UPPER RIGHT": (0.5, 0, 1, 0.5),
            "LOWER LEFT": (0, 0.5, 0.5, 1), "LOWER RIGHT": (0.5, 0.5, 1, 1)}


def numbered(name, rows, prefix):
    """The rectangles of a mosaic named by its numbered sub-cells."""
    rects = []
    for digit in name[len(prefix):]:
        index = int(digit) - 1
        row, column = divmod(index, 2)
        rects.append((column / 2, row / rows, (column + 1) / 2, (row + 1) / rows))
    return rects


def shapes():
    out = {}
    for code, (name, rects) in BLOCKS.items():
        if unicodedata.name(chr(code)) != name:
            sys.exit("gen_mosaic: U+%04X is %s, not %s" % (code, unicodedata.name(chr(code)), name))
        out[code] = rects
    for code in range(0x2596, 0x25A0):
        name = unicodedata.name(chr(code))
        out[code] = [QUADRANT[part] for part in name[len("QUADRANT "):].split(" AND ")]
    for code in list(range(0x1FB00, 0x1FB40)) + list(range(0x1CD00, 0x1CE00)):
        name = unicodedata.name(chr(code), "")
        if name.startswith("BLOCK SEXTANT-"):
            out[code] = numbered(name, 3, "BLOCK SEXTANT-")
        elif name.startswith("BLOCK OCTANT-"):
            out[code] = numbered(name, 4, "BLOCK OCTANT-")
    return out


def pattern(rects, rows):
    """The pattern the rectangles fill in a grid of rows, or None when they
    cover some sub-cell in part."""
    bits = 0
    for index in range(2 * rows):
        row, column = divmod(index, 2)
        x0, y0, x1, y1 = column / 2, row / rows, (column + 1) / 2, (row + 1) / rows
        area = sum(max(0, min(x1, r[2]) - max(x0, r[0])) * max(0, min(y1, r[3]) - max(y0, r[1])) for r in rects)
        whole = (x1 - x0) * (y1 - y0)
        if abs(area - whole) < 1e-9:
            bits |= 1 << index
        elif area > 1e-9:
            return None
    return bits


def main():
    every = shapes()
    tables, own = {}, {}
    for rows, _, _ in GRIDS:
        table = {}
        for code, rects in sorted(every.items()):
            bits = pattern(rects, rows)
            if bits is None:
                continue
            if bits in table:
                sys.exit("gen_mosaic: pattern %d of %d rows is both U+%04X and U+%04X" % (bits, rows, table[bits], code))
            table[bits] = code
            own.setdefault(code, (rows, bits))
        missing = [bits for bits in range(1 << (2 * rows)) if bits not in table]
        if missing:
            sys.exit("gen_mosaic: %d rows lacks patterns %s" % (rows, missing))
        tables[rows] = table

    w = sys.stdout.write
    w("""/*
 * boxos_mosaic.h — Unicode's block mosaics as the patterns they fill.
 * GENERATED by tools/gen_mosaic.py from the character names of Unicode %s;
 * not edited by hand.
 *
 * A mosaic character fills some of a cell's sub-cells, two across and two,
 * three or four down, in the foreground, and leaves the rest to the
 * background. Its pattern is a bit for each sub-cell, in reading order from
 * the top left: bit 0 the top left, bit 1 the top right, bit 2 the left of
 * the second row, and so on. Every pattern of each grid is a character — the
 * quadrants with the halves (two rows), the sextants (three), the octants
 * with the blocks that complete them (four); the empty pattern is the space
 * and the whole one the full block — so a cell of two colours on a grid is
 * always one character.
 *
 * BoxMosaicShapeOf answers the other way: the grid a character is a pattern
 * of, its coarsest, and the pattern. The includer supplies uint32_t and
 * uint8_t.
 */
#ifndef BOXOS_MOSAIC_H
#define BOXOS_MOSAIC_H

""" % unicodedata.unidata_version)
    for rows, name, macro in GRIDS:
        w("#define BOXOS_MOSAIC_%s_ROWS %d\n" % (macro, rows))
    w("\n")
    for rows, name, _ in GRIDS:
        table = tables[rows]
        w("static const uint32_t BoxMosaic%s[%d] = {\n" % (name, len(table)))
        for start in range(0, len(table), 8):
            w("    " + " ".join("0x%05X," % table[bits] for bits in range(start, min(start + 8, len(table)))) + "\n")
        w("};\n\n")
    w("""typedef struct BoxMosaicShape {
    uint32_t Character;
    uint8_t  Rows;       /* the grid it is a pattern of: 2, 3 or 4 rows */
    uint8_t  Pattern;
} BoxMosaicShape;

static const BoxMosaicShape BoxMosaicShapes[] = {
""")
    for code in sorted(own):
        rows, bits = own[code]
        w("    {0x%05X, %d, 0x%02X},\n" % (code, rows, bits))
    w("""};

/* The grid Character is a pattern of, and the pattern; 0 when it is no
 * mosaic. Sorted by character and found by halves. */
static inline int BoxMosaicShapeOf(uint32_t Character, uint8_t *Rows, uint8_t *Pattern)
{
    unsigned Low = 0, High = sizeof(BoxMosaicShapes) / sizeof(BoxMosaicShapes[0]);
    while (Low < High) {
        const unsigned Middle = Low + (High - Low) / 2;
        if (BoxMosaicShapes[Middle].Character < Character) {
            Low = Middle + 1;
        } else if (BoxMosaicShapes[Middle].Character > Character) {
            High = Middle;
        } else {
            *Rows    = BoxMosaicShapes[Middle].Rows;
            *Pattern = BoxMosaicShapes[Middle].Pattern;
            return 1;
        }
    }
    return 0;
}

#endif
""")


main()
