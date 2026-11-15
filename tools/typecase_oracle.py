#!/usr/bin/env python3
"""typecase oracle — a TrueType font as Typecase must read it, said by a second
reader written apart from it (src/userspace/typecase), in the words
tools/typecase_stand.cpp's `outlines` says it in:

    tools/typecase_oracle.py FONT [INDEX]              the font, said
    tools/typecase_oracle.py FONT INDEX --held SAID    SAID (the stand's words) held to it

the line's measures, every character the chosen map names and its glyph (the
map walked range by range here, where the stand asks Find of every
character), and every glyph's advance and outline: its contours as M, L and
Q steps in font units to 1/64, implied points made explicit before the walk
here, where the stand makes them as it walks. Only a font that reads is held
to it: a fault is said and the oracle stops.

Held, every line must be the oracle's word for word but for a coordinate
within a step of 1/64: a scaled component's point is a product the stand
takes in single precision and this in double, and where the true value
falls on the half between two steps the two may round it apart.
"""
import math
import struct
import sys

MOST_POINTS = 65535
DEEPEST = 16


class Broken(Exception):
    pass


def be(fmt, data, at):
    size = struct.calcsize(fmt)
    if at < 0 or at + size > len(data):
        raise Broken("read past the end at %d" % at)
    return struct.unpack_from(">" + fmt, data, at)


class Font:
    def __init__(self, data, index=0):
        self.data = data
        start = 0
        if data[:4] == b"ttcf":
            (count,) = be("I", data, 8)
            if index >= count:
                raise Broken("no font %d" % index)
            (start,) = be("I", data, 12 + 4 * index)
        (version, tables) = be("IH", data, start)
        if version not in (0x00010000, 0x74727565):
            raise Broken("not TrueType")
        self.tables = {}
        for i in range(tables):
            tag, _, offset, length = be("4sIII", data, start + 12 + 16 * i)
            name = tag.decode("latin-1")
            if name not in self.tables:
                self.tables[name] = data[offset:offset + length]
        head, maxp, hhea = self.tables["head"], self.tables["maxp"], self.tables["hhea"]
        (self.upem,) = be("H", head, 18)
        (loca_format,) = be("h", head, 50)
        (self.glyphs,) = be("H", maxp, 4)
        self.ascender, self.descender, self.gap = be("hhh", hhea, 4)
        (self.advances,) = be("H", hhea, 34)
        self.xheight = self.capheight = 0
        os2 = self.tables.get("OS/2", b"")
        if len(os2) >= 78 and be("H", os2, 62)[0] & 0x80:
            self.ascender, self.descender, self.gap = be("hhh", os2, 68)
        if len(os2) >= 90 and be("H", os2, 0)[0] >= 2:
            self.xheight, self.capheight = be("hh", os2, 86)
        loca = self.tables["loca"]
        if loca_format == 0:
            self.places = [2 * v for v in be("%dH" % (self.glyphs + 1), loca, 0)]
        else:
            self.places = list(be("%dI" % (self.glyphs + 1), loca, 0))
        self.glyf = self.tables["glyf"]
        self.mapping = self.read_map(self.tables["cmap"])

    # --- the character map -------------------------------------------------

    def read_map(self, cmap):
        (records,) = be("H", cmap, 2)
        ranks = {12: 6, 13: 5, 4: 4, 6: 3, 0: 2}
        best = None
        for i in range(records):
            platform, encoding, offset = be("HHI", cmap, 4 + 8 * i)
            if offset + 2 > len(cmap):
                continue
            (fmt,) = be("H", cmap, offset)
            unicode = (platform == 0 and encoding != 5) or (platform == 3 and encoding in (1, 10))
            symbol = platform == 3 and encoding == 0
            rank = (1 if fmt in (4, 6, 12) else 0) if symbol else (ranks.get(fmt, 0) if unicode else 0)
            if rank == 0 or (best and rank <= best[0]):
                continue
            try:
                ranges = self.ranges(cmap[offset:], fmt)
            except Broken:
                continue
            best = (rank, ranges, symbol)
        if not best:
            raise Broken("no Unicode map")
        _, ranges, symbol = best
        mapping = {}
        for character, glyph in ranges:
            if 0 < glyph < self.glyphs:
                mapping[character] = glyph
        if symbol:
            for character in range(256):
                if character not in mapping and 0xF000 + character in mapping:
                    mapping[character] = mapping[0xF000 + character]
        return mapping

    def ranges(self, sub, fmt):
        """Every (character, glyph) the subtable names, after checking that it
        holds — the order a search needs, its arrays within the table."""
        out = []
        if fmt == 0:
            be("256B", sub, 6)
            out = [(c, sub[6 + c]) for c in range(256)]
        elif fmt == 4:
            (seg2,) = be("H", sub, 6)
            if seg2 == 0 or seg2 % 2:
                raise Broken("segments")
            n = seg2 // 2
            ends = be("%dH" % n, sub, 14)
            starts = be("%dH" % n, sub, 16 + seg2)
            deltas = be("%dH" % n, sub, 16 + 2 * seg2)
            rangeoffs = be("%dH" % n, sub, 16 + 3 * seg2)
            for i in range(n):
                if starts[i] > ends[i] or (i and ends[i] <= ends[i - 1]):
                    raise Broken("order")
            for i in range(n):
                for c in range(starts[i], ends[i] + 1):
                    if rangeoffs[i] == 0:
                        g = (c + deltas[i]) & 0xFFFF
                    else:
                        at = 16 + 3 * seg2 + 2 * i + rangeoffs[i] + 2 * (c - starts[i])
                        g = be("H", sub, at)[0] if at + 2 <= len(sub) else 0
                        g = (g + deltas[i]) & 0xFFFF if g else 0
                    out.append((c, g))
        elif fmt == 6:
            first, count = be("HH", sub, 6)
            out = [(first + i, g) for i, g in enumerate(be("%dH" % count, sub, 10))]
        elif fmt in (12, 13):
            (groups,) = be("I", sub, 12)
            if groups > len(sub) // 12:
                raise Broken("groups")
            last = -1
            spans = []
            for i in range(groups):
                s, e, g = be("III", sub, 16 + 12 * i)
                if s > e or e > 0x10FFFF or s <= last:
                    raise Broken("order")
                last = e
                spans.append((s, e, g))
            for s, e, g in spans:
                for c in range(s, e + 1):
                    glyph = g if fmt == 13 else g + (c - s)
                    out.append((c, glyph if glyph <= 0xFFFF else 0))
        return out

    # --- glyphs ------------------------------------------------------------

    def advance(self, glyph):
        return be("H", self.tables["hmtx"], 4 * min(glyph, self.advances - 1))[0]

    def contours(self, glyph, within=()):
        """The glyph's contours, each a list of [x, y, on]."""
        if glyph in within:
            raise Broken("cycle")
        if len(within) > DEEPEST:
            raise Broken("deep")
        if glyph >= self.glyphs:
            raise Broken("past the glyphs")
        start, end = self.places[glyph], self.places[glyph + 1]
        if end < start or end > len(self.glyf):
            raise Broken("loca")
        rec = self.glyf[start:end]
        if not rec:
            return []
        (count,) = be("h", rec, 0)
        if count >= 0:
            return self.simple(rec, count)
        return self.composite(rec, within + (glyph,))

    def simple(self, rec, count):
        ends = be("%dH" % count, rec, 10)
        for i in range(1, count):
            if ends[i] <= ends[i - 1]:
                raise Broken("ends")
        points = ends[-1] + 1 if count else 0
        if points > MOST_POINTS:
            raise Broken("points")
        at = 10 + 2 * count
        at += 2 + be("H", rec, at)[0]
        flags = []
        while len(flags) < points:
            (f,) = be("B", rec, at)
            at += 1
            times = 1
            if f & 8:
                times += be("B", rec, at)[0]
                at += 1
            if len(flags) + times > points:
                raise Broken("repeat")
            flags += [f] * times
        coords = []
        for short, same in ((2, 16), (4, 32)):
            value, axis = 0, []
            for f in flags:
                if f & short:
                    (d,) = be("B", rec, at)
                    at += 1
                    value += d if f & same else -d
                elif not f & same:
                    value += be("h", rec, at)[0]
                    at += 2
                axis.append(value)
            coords.append(axis)
        out, first = [], 0
        for e in ends:
            out.append([[float(coords[0][i]), float(coords[1][i]), bool(flags[i] & 1)] for i in range(first, e + 1)])
            first = e + 1
        return out

    def composite(self, rec, within):
        out, at, more = [], 10, True
        while more:
            flags, part = be("HH", rec, at)
            at += 4
            offsets = flags & 2
            if flags & 1:
                a1, a2 = be("hh" if offsets else "HH", rec, at)
                at += 4
            else:
                a1, a2 = be("bb" if offsets else "BB", rec, at)
                at += 2
            m = [1.0, 0.0, 0.0, 1.0]  # xx, yx, xy, yy
            scaled = True
            if flags & 0x80:
                m = [v / 16384 for v in be("hhhh", rec, at)]
                at += 8
            elif flags & 0x40:
                sx, sy = be("hh", rec, at)
                m = [sx / 16384, 0.0, 0.0, sy / 16384]
                at += 4
            elif flags & 8:
                (s,) = be("h", rec, at)
                m = [s / 16384, 0.0, 0.0, s / 16384]
                at += 2
            else:
                scaled = False
            parts = self.contours(part, within)
            for contour in parts:
                for p in contour:
                    x, y = p[0], p[1]
                    p[0], p[1] = m[0] * x + m[2] * y, m[1] * x + m[3] * y
            if offsets:
                dx, dy = float(a1), float(a2)
                if flags & 0x800 and not flags & 0x1000 and scaled:
                    dx *= math.hypot(m[0], m[2])
                    dy *= math.hypot(m[3], m[1])
            else:
                gathered = [p for c in out for p in c]
                own = [p for c in parts for p in c]
                if a1 >= len(gathered) or a2 >= len(own):
                    raise Broken("pin")
                dx, dy = gathered[a1][0] - own[a2][0], gathered[a1][1] - own[a2][1]
            for contour in parts:
                for p in contour:
                    p[0] += dx
                    p[1] += dy
            out += parts
            if sum(len(c) for c in out) > MOST_POINTS:
                raise Broken("points")
            more = flags & 0x20
        return out


def mid(a, b):
    return [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, True]


def steps(contour):
    """A contour as steps: its implied points first made explicit, the walk
    begun where Typecase begins it — the first point on the curve, else the
    last, else the point between them — and closed by a curve when it ends
    on a control, the closing line left unsaid."""
    n = len(contour)
    if n < 2:
        return []
    full, last = [], 0
    for i, p in enumerate(contour):
        last = len(full)
        full.append(p)
        q = contour[(i + 1) % n]
        if not p[2] and not q[2]:
            full.append(mid(p, q))
    if contour[0][2]:
        begin = 0
    elif contour[-1][2]:
        begin = last
    else:
        begin = len(full) - 1  # the point made between the last and the first
    walk = full[begin:] + full[:begin]
    said = [("M", walk[0])]
    i = 1
    while i < len(walk):
        if walk[i][2]:
            said.append(("L", walk[i]))
            i += 1
        else:
            end = walk[i + 1] if i + 1 < len(walk) else walk[0]
            said.append(("Q", walk[i], end))
            i += 2
    return said


def num(v):
    return "%.3f" % (round(v * 64) / 64)


def say(font):
    yield ("font glyphs=%d upem=%d ascender=%d descender=%d gap=%d xheight=%d capheight=%d"
           % (font.glyphs, font.upem, font.ascender, font.descender, font.gap, font.xheight, font.capheight))
    for c in sorted(font.mapping):
        yield "U+%04X %d" % (c, font.mapping[c])
    for g in range(font.glyphs):
        yield "glyph %d advance=%d" % (g, font.advance(g))
        for contour in font.contours(g):
            for step in steps(contour):
                yield " " + step[0] + "".join(" " + num(p[0]) + " " + num(p[1]) for p in step[1:])


def alike(mine, theirs):
    a, b = mine.split(), theirs.split()
    if len(a) != len(b) or not a or a[0] != b[0] or a[0] not in ("M", "L", "Q"):
        return mine == theirs
    return all(abs(round(float(x) * 64) - round(float(y) * 64)) <= 1 for x, y in zip(a[1:], b[1:]))


def held(font, path):
    theirs = open(path).read().split("\n")
    if theirs and theirs[-1] == "":
        theirs.pop()
    mine = list(say(font))
    differ = [i for i in range(min(len(mine), len(theirs))) if not alike(mine[i], theirs[i])]
    if len(mine) != len(theirs):
        differ.append(min(len(mine), len(theirs)))
    if differ:
        i = differ[0]
        print("differs at line %d of %d (%d lines differ): oracle %r, stand %r"
              % (i + 1, len(mine), len(differ), mine[i] if i < len(mine) else None,
                 theirs[i] if i < len(theirs) else None))
        return 1
    print("held: %d lines" % len(mine))
    return 0


def main():
    path = sys.argv[1]
    index = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    font = Font(open(path, "rb").read(), index)
    if len(sys.argv) > 4 and sys.argv[3] == "--held":
        sys.exit(held(font, sys.argv[4]))
    for line in say(font):
        print(line)


if __name__ == "__main__":
    try:
        main()
    except Broken as fault:
        print("broken: %s" % fault)
        sys.exit(1)
