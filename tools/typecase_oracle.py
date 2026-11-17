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


# --- what the face is, and its kerning -------------------------------------
# Read past a table's end as 0, as Typecase reads it: a font that lies kerns
# nothing rather than breaks.

def u16(data, at):
    return struct.unpack_from(">H", data, at)[0] if 0 <= at and at + 2 <= len(data) else 0


def s16(data, at):
    v = u16(data, at)
    return v - 65536 if v >= 32768 else v


def u32(data, at):
    return struct.unpack_from(">I", data, at)[0] if 0 <= at and at + 4 <= len(data) else 0


def name_of(table, wanted):
    best = None
    count, storage = u16(table, 2), u16(table, 4)
    for i in range(count if len(table) >= 6 else 0):
        at = 6 + 12 * i
        if at + 12 > len(table):
            break
        platform, encoding, language, nid, length, offset = struct.unpack_from(">6H", table, at)
        if nid != wanted:
            continue
        if platform == 3 and encoding in (1, 10):
            rank = 4 if language == 0x409 else 3
        elif platform == 0:
            rank = 2
        elif platform == 1 and encoding == 0 and language == 0:
            rank = 1
        else:
            rank = 0
        start = storage + offset
        if rank and start + length <= len(table) and (best is None or rank > best[0]):
            best = (rank, table[start:start + length], platform != 1)
    if not best:
        return ""
    raw = best[1]
    if best[2]:
        return raw[:len(raw) // 2 * 2].decode("utf-16-be", errors="replace")
    return raw.decode("mac_roman")


def style_of(font):
    names = font.tables.get("name", b"")
    family = name_of(names, 16) or name_of(names, 1)
    own = name_of(names, 17) or name_of(names, 2)
    os2, head = font.tables.get("OS/2", b""), font.tables["head"]
    weight = u16(os2, 4) if len(os2) >= 6 else 0
    if 1 <= weight <= 9:
        weight *= 100
    selection = u16(os2, 62) if len(os2) >= 64 else 0
    mac = u16(head, 44)
    if not 1 <= weight <= 1000:
        weight = 700 if selection & 0x20 or mac & 1 else 400
    italic = bool(selection & 0x201) or bool(mac & 2)
    post = font.tables.get("post", b"")
    under = (s16(post, 8), s16(post, 10)) if len(post) >= 12 else (0, 0)
    strike = (s16(os2, 28), s16(os2, 26)) if len(os2) >= 30 else (0, 0)
    return family, own, weight, italic, under, strike


def coverage_index(cov, glyph):
    fmt = u16(cov, 0)
    if fmt == 1:
        for i in range(u16(cov, 2)):
            if u16(cov, 4 + 2 * i) == glyph:
                return i
    elif fmt == 2:
        for i in range(u16(cov, 2)):
            start, end, first = u16(cov, 4 + 6 * i), u16(cov, 6 + 6 * i), u16(cov, 8 + 6 * i)
            if start <= glyph <= end:
                return first + glyph - start
    return -1


def class_of(cd, glyph):
    fmt = u16(cd, 0)
    if fmt == 1:
        first, count = u16(cd, 2), u16(cd, 4)
        return u16(cd, 6 + 2 * (glyph - first)) if first <= glyph < first + count else 0
    if fmt == 2:
        for i in range(u16(cd, 2)):
            if u16(cd, 4 + 6 * i) <= glyph <= u16(cd, 6 + 6 * i):
                return u16(cd, 8 + 6 * i)
    return 0


def value(data, at, fmt):
    shift = advance = 0
    if fmt & 1:
        shift = s16(data, at)
        at += 2
    if fmt & 2:
        at += 2
    if fmt & 4:
        advance = s16(data, at)
    return shift, advance


def pair(sub, first, second):
    """A pair adjustment subtable's moves for the pair, or None when it has none."""
    index = coverage_index(sub[u16(sub, 2):], first)
    if index < 0:
        return None
    f1, f2 = u16(sub, 4), u16(sub, 6)
    n1, n2 = 2 * bin(f1 & 0xFF).count("1"), 2 * bin(f2 & 0xFF).count("1")
    if u16(sub, 0) == 1:
        if index >= u16(sub, 8):
            return None
        pairs = sub[u16(sub, 10 + 2 * index):]
        for i in range(u16(pairs, 0)):
            at = 2 + i * (2 + n1 + n2)
            if u16(pairs, at) == second:
                return value(pairs, at + 2, f1) + value(pairs, at + 2 + n1, f2)
        return None
    if u16(sub, 0) == 2:
        c1, c2 = class_of(sub[u16(sub, 8):], first), class_of(sub[u16(sub, 10):], second)
        if c1 >= u16(sub, 12) or c2 >= u16(sub, 14):
            return None
        at = 16 + (c1 * u16(sub, 14) + c2) * (n1 + n2)
        return value(sub, at, f1) + value(sub, at + n1, f2)
    return None


class Kerning:
    def __init__(self, font):
        self.scripts, self.table = {}, []
        gpos = font.tables.get("GPOS", b"")
        if len(gpos) >= 10 and u16(gpos, 0) == 1:
            scripts, features, lookups = gpos[u16(gpos, 4):], gpos[u16(gpos, 6):], gpos[u16(gpos, 8):]
            for i in range(u16(scripts, 0)):
                tag = scripts[2 + 6 * i:6 + 6 * i].decode("latin-1")
                script = scripts[u16(scripts, 6 + 6 * i):]
                system = u16(script, 0) or (u16(script, 6) if u16(script, 2) else 0)
                if not system:
                    continue
                langsys = script[system:]
                wanted = ([u16(langsys, 2)] if u16(langsys, 2) != 0xFFFF else []) + \
                         [u16(langsys, 6 + 2 * k) for k in range(u16(langsys, 4))]
                chosen = set()
                for f in wanted:
                    if f >= u16(features, 0) or features[2 + 6 * f:6 + 6 * f] != b"kern":
                        continue
                    feature = features[u16(features, 6 + 6 * f):]
                    chosen.update(u16(feature, 4 + 2 * k) for k in range(u16(feature, 2)))
                kept = []
                for index in sorted(chosen):
                    if index >= u16(lookups, 0):
                        continue
                    lookup = lookups[u16(lookups, 2 + 2 * index):]
                    subs = []
                    for k in range(u16(lookup, 4)):
                        sub = lookup[u16(lookup, 6 + 2 * k):]
                        if u16(lookup, 0) == 9:
                            if u16(sub, 0) != 1 or u16(sub, 2) != 2:
                                continue
                            sub = sub[u32(sub, 4):]
                        elif u16(lookup, 0) != 2:
                            break
                        if sub:
                            subs.append(sub)
                    if subs:
                        kept.append(subs)
                if kept:
                    self.scripts[tag] = kept
        if self.scripts:
            return
        kern = font.tables.get("kern", b"")
        apple = u32(kern, 0) == 0x00010000
        count, at = (u32(kern, 4), 8) if apple else ((u16(kern, 2), 4) if u16(kern, 0) == 0 else (0, 4))
        head = 8 if apple else 6
        while count > 0 and at + head <= len(kern):
            length = u32(kern, at) if apple else u16(kern, at + 2)
            coverage = u16(kern, at + 4)
            body = at + head
            if apple:
                fmt0, wanted, replaces = coverage & 0xFF == 0, not coverage & 0xE000, False
            else:
                fmt0, wanted, replaces = coverage >> 8 == 0, coverage & 1 and not coverage & 6, bool(coverage & 8)
            if fmt0 and wanted and body + 8 <= len(kern):
                pairs = {}
                for i in range(min(u16(kern, body), (len(kern) - body - 8) // 6)):
                    at2 = body + 8 + 6 * i
                    pairs[(u16(kern, at2), u16(kern, at2 + 2))] = s16(kern, at2 + 4)
                self.table.append((pairs, replaces))
            npairs = u16(kern, body) * 6 + 8 if fmt0 else 0
            at = body + max(npairs, length - head if length > head else 0)
            if length == 0 and npairs == 0:
                break
            count -= 1

    def of(self, first, second, tag):
        moves = [0, 0, 0, 0]
        if self.scripts:
            for each in (tag, "DFLT", "dflt", "latn"):
                if each in self.scripts:
                    for subs in self.scripts[each]:
                        for sub in subs:
                            got = pair(sub, first, second)
                            if got is not None:
                                moves = [m + g for m, g in zip(moves, got)]
                                break
                    break
            return moves
        for pairs, replaces in self.table:
            if (first, second) in pairs:
                moves[1] = pairs[(first, second)] if replaces else moves[1] + pairs[(first, second)]
        return moves


KERNED = (("latn", list(range(0x21, 0x7F))),
          ("cyrl", [0x401] + list(range(0x410, 0x450)) + [0x451]),
          ("grek", list(range(0x391, 0x3AA)) + list(range(0x3B1, 0x3CA))))


def say(font):
    yield ("font glyphs=%d upem=%d ascender=%d descender=%d gap=%d xheight=%d capheight=%d"
           % (font.glyphs, font.upem, font.ascender, font.descender, font.gap, font.xheight, font.capheight))
    family, own, weight, italic, under, strike = style_of(font)
    yield "style family=%s name=%s weight=%d italic=%d" % (family, own, weight, italic)
    yield "lines underline=%d,%d strike=%d,%d" % (under + strike)
    kerning = Kerning(font)
    for tag, characters in KERNED:
        mapped = [c for c in characters if c in font.mapping]
        for a in mapped:
            for b in mapped:
                moves = kerning.of(font.mapping[a], font.mapping[b], tag)
                if any(moves):
                    yield "kern %s U+%04X U+%04X %d %d %d %d" % ((tag, a, b) + tuple(moves))
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
