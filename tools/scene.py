#!/usr/bin/env python3
"""scene.py — a scene (src/include/boxos_scene.h) packed from its text, and
what a packed one holds.

  scene.py pack SOURCE OUT   the text of a scene (src/userspace/scenes/) packed
                             into the form Gangway and the display play
  scene.py read FILE         what a packed scene holds, a line each

The numbers of the form are read out of boxos_scene.h and boxos_brush.h, its
records held to the sizes boxos_scene.h asserts. Nothing here is a second copy
of what the loader or the display read.

The text — a statement a line, a remark from a # on its own to the end of the
line; places in the scene's units, colours #RRGGBB or #AARRGGBB, numbers as
they are:

  scene units=U paper=#RRGGBB enter=MS wait=MS leave=MS
  paint NAME solid COLOUR
  paint NAME radial matrix=A,B,C,D,E,F center=X,Y radius=R stops=AT:COLOUR,...
  paint NAME linear from=X,Y to=X,Y stops=AT:COLOUR,...
  group NAME [FIGURE...]
  ellipse NAME center=X,Y radius=R[,R] paint=NAME [FIGURE...]
  rect NAME box=X,Y,W,H paint=NAME [FIGURE...]
  path NAME paint=NAME [FIGURE...] (shape=PATH | d=DATA...)
  move NAME ACT WHAT MS:VALUE[:EASE] ...

  FIGURE: in=GROUP pivot=X,Y at=X,Y scale=S wide=S tall=S turn=TURNS opacity=0..255
  ACT: enter wait leave; WHAT: x y (where the pivot is put) wide tall scale turn opacity;
  EASE: linear in out inout step.

A path's DATA is an SVG path's, to the end of the line — M L H V C S Q T Z,
absolute and relative, no arcs — or shape= draws the pieces of a path said
before it again, with paint of its own: a light that keeps to a letter's
edges. A paint's places are its figure's own. A figure's pivot is in its own places
and stays where it is unless at= puts it elsewhere in its group. The wait
act's tracks must begin where enter left the figure and come back to where
they began — a turn by whole turns — so its round has no seam.
"""

import os
import re
import struct
import sys

INCLUDE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "include")

HEAD = struct.Struct("<8s12I")
PAINT = struct.Struct("<4I6i")
STOP = struct.Struct("<2I")
FIGURE = struct.Struct("<3I4i2i6i")
PIECE = struct.Struct("<I2i")
TRACK = struct.Struct("<5I")
KEY = struct.Struct("<IiI")
RECORDS = {"BoxSceneHead": HEAD, "BoxScenePaint": PAINT, "BoxBrushStop": STOP, "BoxSceneFigure": FIGURE,
           "BoxScenePiece": PIECE, "BoxSceneTrack": TRACK, "BoxSceneKey": KEY}

ACTS = ("enter", "wait", "leave")
WHATS = ("x", "y", "wide", "tall", "turn", "opacity")
EASES = ("linear", "in", "out", "inout", "step")
KINDS = {"group": "BOX_SCENE_GROUP", "ellipse": "BOX_SCENE_ELLIPSE", "rect": "BOX_SCENE_RECT", "path": "BOX_SCENE_PATH"}
PATH_WORD = re.compile(r"[MmLlHhVvCcSsQqTtZzAa]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")


class Refused(Exception):
    pass


def numbers():
    """Every #define of boxos_brush.h and boxos_scene.h, worked out, and the
    sizes boxos_scene.h asserts of its records."""
    names = {}
    for header in ("boxos_brush.h", "boxos_scene.h"):
        text = open(os.path.join(INCLUDE, header)).read()
        for name, value in re.findall(r"^#define\s+(BOX_\w+)\s+(.+?)\s*(?:/\*.*)?$", text, re.M):
            if value.startswith('"'):
                names[name] = value.strip('"').encode()
                continue
            expr = re.sub(r"\b(0x[0-9A-Fa-f]+|\d+)u\b", r"\1", value)
            names[name] = eval(expr, {"__builtins__": {}}, dict(names))
        for record, size in re.findall(r"sizeof\((\w+)\)\s*==\s*(\d+)", text):
            if record in RECORDS and RECORDS[record].size != int(size):
                sys.exit(f"scene: {header} asserts {record} of {size} bytes, this tool packs {RECORDS[record].size}")
    return names


def colour(token):
    found = re.fullmatch(r"#([0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})", token)
    if not found:
        raise Refused(f"{token} is no colour")
    raw = int(found.group(1), 16)
    return raw | 0xFF000000 if len(found.group(1)) == 6 else raw


def floats(token, count):
    try:
        got = [float(t) for t in token.split(",")]
    except ValueError:
        raise Refused(f"{token} is not {count} number(s)")
    if len(got) != count:
        raise Refused(f"{token} is not {count} number(s)")
    return got


def outlines(data):
    """An SVG path's data as (verb, x, y) pieces in its own units, every
    curve absolute and whole: MOVE, LINE, QUAD twice, CUBIC thrice."""
    words, at, pieces = PATH_WORD.findall(data), 0, []
    if "".join(words).replace(" ", "") != re.sub(r"[\s,]+", "", data):
        raise Refused("a path's data holds what is no command and no number")
    x = y = sx = sy = 0.0
    last, pull, command, pending = None, None, None, None

    def take(count):
        nonlocal at
        got = words[at:at + count]
        if len(got) < count or any(w.isalpha() for w in got):
            raise Refused(f"a path's {command} lacks its numbers")
        at += count
        return [float(w) for w in got]

    def begin():
        nonlocal pending
        if pending is not None:
            pieces.append(("MOVE",) + pending)
            pending = None

    while at < len(words):
        if words[at].isalpha():
            command = words[at]
            at += 1
            if command in "Aa":
                raise Refused("a path's arcs are not taken: flatten them to curves first")
        elif command is None or command in "Zz":
            raise Refused("a path's data begins with a number, or has numbers after Z")
        rel = command.islower()
        c = command.upper()
        ox, oy = (x, y) if rel else (0.0, 0.0)
        if c == "M":
            px, py = take(2)
            x, y = ox + px, oy + py
            sx, sy, pending = x, y, None
            pieces.append(("MOVE", x, y))
            command = "l" if rel else "L"
        elif c == "Z":
            x, y, pending = sx, sy, (sx, sy)
        elif c in "LHV":
            begin()
            if c == "L":
                px, py = take(2)
                x, y = ox + px, oy + py
            elif c == "H":
                x = ox + take(1)[0]
            else:
                y = oy + take(1)[0]
            pieces.append(("LINE", x, y))
        elif c in "CS":
            begin()
            if c == "C":
                c1 = take(2)
                c1 = (ox + c1[0], oy + c1[1])
            else:
                c1 = (2 * x - pull[0], 2 * y - pull[1]) if last in "CS" else (x, y)
            c2, end = take(2), take(2)
            c2, end = (ox + c2[0], oy + c2[1]), (ox + end[0], oy + end[1])
            pieces += [("CUBIC",) + c1, ("CUBIC",) + c2, ("CUBIC",) + end]
            pull, (x, y) = c2, end
        elif c in "QT":
            begin()
            if c == "Q":
                c1 = take(2)
                c1 = (ox + c1[0], oy + c1[1])
            else:
                c1 = (2 * x - pull[0], 2 * y - pull[1]) if last in "QT" else (x, y)
            end = take(2)
            end = (ox + end[0], oy + end[1])
            pieces += [("QUAD",) + c1, ("QUAD",) + end]
            pull, (x, y) = c1, end
        else:
            raise Refused(f"{command} is no command of a path")
        last = c
    if not pieces or pieces[0][0] != "MOVE":
        raise Refused("a path begins with M")
    # A move the pen makes with nothing drawn after it draws nothing.
    drawn = [p for i, p in enumerate(pieces)
             if p[0] != "MOVE" or (i + 1 < len(pieces) and pieces[i + 1][0] != "MOVE")]
    if len(drawn) < 2:
        raise Refused("a path draws nothing")
    return drawn


class Scene:
    def __init__(self, n):
        self.n = n
        self.units = None
        self.paper = 0
        self.acts = [0, 0, 0]
        self.paints, self.paint_names, self.stops = [], {}, []
        self.figures, self.figure_names = [], {}
        self.pieces = []
        self.tracks = {}

    def place(self, value):
        return round(value * self.n["BOX_BRUSH_ONE"])

    def options(self, words, allowed):
        got = {}
        for word in words:
            key, eq, value = word.partition("=")
            if not eq or key not in allowed:
                raise Refused(f"{word} is no option here ({', '.join(allowed)})")
            if key in got:
                raise Refused(f"{key} is said twice")
            got[key] = value
        return got

    def statement_scene(self, words):
        o = self.options(words, ("units", "paper", "enter", "wait", "leave"))
        if set(o) != {"units", "paper", "enter", "wait", "leave"}:
            raise Refused("scene says units, paper, enter, wait and leave")
        self.units = int(o["units"])
        self.paper = colour(o["paper"]) & 0xFFFFFF
        self.acts = [int(o[a]) for a in ACTS]

    def shade_space(self, a, b, c, d, e, f, cx, cy, r):
        one = self.n["BOX_BRUSH_ONE"]
        return [round(one * a * r), round(one * b * r), round(one * c * r), round(one * d * r),
                round(one * (a * cx + c * cy + e)), round(one * (b * cx + d * cy + f))]

    def given_stops(self, token):
        out = []
        for item in token.split(","):
            at, colon, ink = item.partition(":")
            if not colon:
                raise Refused(f"{item} is no stop AT:COLOUR")
            out.append((float(at), colour(ink)))
        return out

    def statement_paint(self, words):
        if len(words) < 2:
            raise Refused("paint says its name and kind")
        name, kind, rest = words[0], words[1], words[2:]
        if name in self.paint_names:
            raise Refused(f"paint {name} is said twice")
        n = self.n
        if kind == "solid":
            if len(rest) != 1:
                raise Refused("a solid paint is one colour")
            paint = (n["BOX_BRUSH_PAINT_SOLID"], colour(rest[0]), [], [0] * 6)
        elif kind == "radial":
            o = self.options(rest, ("matrix", "center", "radius", "stops"))
            paint = (n["BOX_BRUSH_PAINT_RADIAL"], 0, self.given_stops(o["stops"]),
                     self.shade_space(*floats(o["matrix"], 6), *floats(o["center"], 2), *floats(o["radius"], 1)))
        elif kind == "linear":
            o = self.options(rest, ("from", "to", "stops"))
            (x0, y0), (x1, y1) = floats(o["from"], 2), floats(o["to"], 2)
            a, b = self.place(x1 - x0), self.place(y1 - y0)
            paint = (n["BOX_BRUSH_PAINT_LINEAR"], 0, self.given_stops(o["stops"]),
                     [a, b, -b, a, self.place(x0), self.place(y0)])
        else:
            raise Refused(f"{kind} is no kind of paint (solid, radial, linear)")
        kind_n, ink, stops, space = paint
        first = len(self.stops)
        for at, stop_ink in stops:
            if not 0.0 <= at <= 1.0:
                raise Refused(f"paint {name}: a stop at {at} lies outside 0 to 1")
            self.stops.append((round(at * n["BOX_BRUSH_UNIT"]), stop_ink))
        self.paint_names[name] = len(self.paints)
        self.paints.append((kind_n, ink, first if stops else 0, len(stops), space))

    def statement_figure(self, kind, words):
        if not words:
            raise Refused(f"{kind} says its name")
        name, rest = words[0], words[1:]
        if name in self.figure_names:
            raise Refused(f"figure {name} is said twice")
        shape_keys = {"group": (), "ellipse": ("center", "radius", "paint"),
                      "rect": ("box", "paint"), "path": ("shape", "d", "paint")}[kind]
        data = None
        if kind == "path":
            ds = [i for i, w in enumerate(rest) if w.startswith("d=")]
            if ds:
                data, rest = " ".join([rest[ds[0]][2:]] + rest[ds[0] + 1:]), rest[:ds[0]]
        o = self.options(rest, shape_keys + ("in", "pivot", "at", "scale", "wide", "tall", "turn", "opacity"))
        n = self.n
        parent = n["BOX_SCENE_NONE"]
        if "in" in o:
            if o["in"] not in self.figure_names:
                raise Refused(f"figure {name} is in {o['in']}, which is no figure said before it")
            parent = self.figure_names[o["in"]]
            if self.figures[parent]["kind"] != n["BOX_SCENE_GROUP"]:
                raise Refused(f"figure {name} is in {o['in']}, which is no group")
        shape, paint = [0, 0, 0, 0], 0
        if kind == "ellipse":
            if "center" not in o or "radius" not in o:
                raise Refused(f"ellipse {name} says its center and radius")
            radius = floats(o["radius"], 2 if "," in o["radius"] else 1)
            shape = [self.place(v) for v in floats(o["center"], 2) + (radius * 2 if len(radius) == 1 else radius)]
        elif kind == "rect":
            if "box" not in o:
                raise Refused(f"rect {name} says its box")
            shape = [self.place(v) for v in floats(o["box"], 4)]
        elif kind == "path":
            if (data is None) == ("shape" not in o):
                raise Refused(f"path {name} says one of shape= or d=")
            if data is None:
                like = self.figure_names.get(o["shape"])
                if like is None or self.figures[like]["kind"] != n["BOX_SCENE_PATH"]:
                    raise Refused(f"path {name} takes the shape of {o['shape']}, which is no path said before it")
                shape = list(self.figures[like]["shape"])
            else:
                got = outlines(data)
                if len(got) > n["BOX_SCENE_PIECES_MOST"]:
                    raise Refused(f"path {name} has {len(got)} pieces, more than {n['BOX_SCENE_PIECES_MOST']}")
                shape = [len(self.pieces), len(got), 0, 0]
                self.pieces += [(n["BOX_SCENE_" + verb], self.place(px), self.place(py)) for verb, px, py in got]
        if kind != "group":
            if o.get("paint") not in self.paint_names:
                raise Refused(f"{kind} {name} is painted with {o.get('paint')}, which is no paint said before it")
            paint = self.paint_names[o["paint"]]
        pivot = [self.place(v) for v in (floats(o["pivot"], 2) if "pivot" in o else [0.0, 0.0])]
        at = [self.place(v) for v in floats(o["at"], 2)] if "at" in o else pivot
        scale = float(o.get("scale", "1"))
        base = [at[0] - pivot[0], at[1] - pivot[1],
                round(float(o.get("wide", scale)) * n["BOX_BRUSH_UNIT"]),
                round(float(o.get("tall", scale)) * n["BOX_BRUSH_UNIT"]),
                round(float(o.get("turn", "0")) * n["BOX_BRUSH_TURN"]),
                int(o.get("opacity", "255"))]
        self.figure_names[name] = len(self.figures)
        self.figures.append({"name": name, "kind": n[KINDS[kind]], "parent": parent, "paint": paint,
                             "shape": shape, "pivot": pivot, "base": base})

    def statement_move(self, words):
        if len(words) < 4:
            raise Refused("move says a figure, an act, what it moves and its keys")
        name, act, what, keys = words[0], words[1], words[2], words[3:]
        if name not in self.figure_names:
            raise Refused(f"move: {name} is no figure said before it")
        if act not in ACTS:
            raise Refused(f"move: {act} is no act (enter, wait, leave)")
        whats = ("wide", "tall") if what == "scale" else (what,)
        if what != "scale" and what not in WHATS:
            raise Refused(f"move: {what} is nothing a track moves ({', '.join(WHATS)}, scale)")
        figure = self.figure_names[name]
        pivot = self.figures[figure]["pivot"]
        n = self.n
        for one in whats:
            index = WHATS.index(one)
            track = []
            for key in keys:
                parts = key.split(":")
                if len(parts) not in (2, 3) or (len(parts) == 3 and parts[2] not in EASES):
                    raise Refused(f"move {name}: {key} is no key MS:VALUE[:EASE]")
                ms, raw = int(parts[0]), float(parts[1])
                if one in ("x", "y"):
                    value = self.place(raw) - pivot[0 if one == "x" else 1]
                elif one in ("wide", "tall"):
                    value = round(raw * n["BOX_BRUSH_UNIT"])
                elif one == "turn":
                    value = round(raw * n["BOX_BRUSH_TURN"])
                else:
                    value = int(raw)
                track.append((ms, value, EASES.index(parts[2]) if len(parts) == 3 else 0))
            where = (ACTS.index(act), figure, index)
            if where in self.tracks:
                raise Refused(f"move {name} {act} {one} is said twice")
            self.tracks[where] = track

    def parse(self, path):
        for number, line in enumerate(open(path), 1):
            words = re.split(r"(?:^|\s)#(?:\s|$)", line, maxsplit=1)[0].split()
            if not words:
                continue
            try:
                if words[0] == "scene":
                    self.statement_scene(words[1:])
                elif self.units is None:
                    raise Refused("the scene's first statement is scene")
                elif words[0] == "paint":
                    self.statement_paint(words[1:])
                elif words[0] in KINDS:
                    self.statement_figure(words[0], words[1:])
                elif words[0] == "move":
                    self.statement_move(words[1:])
                else:
                    raise Refused(f"{words[0]} is no statement")
            except (Refused, ValueError) as why:
                raise Refused(f"{path}, line {number}: {why}")
        if self.units is None or not self.figures:
            raise Refused(f"{path}: no scene, or no figure in it")
        self.hold()

    def hold(self):
        """What the reader takes on trust from the packer: keys within their
        act, the wait round without a seam, and wait taking up where enter
        left off."""
        n, turn = self.n, self.n["BOX_BRUSH_TURN"]
        for (act, figure, what), keys in self.tracks.items():
            name = f"move {self.figures[figure]['name']} {ACTS[act]} {WHATS[what]}"
            ats = [k[0] for k in keys]
            if ats != sorted(ats) or ats[0] < 0 or ats[-1] > self.acts[act]:
                raise Refused(f"{name}: its keys do not rise within the act's {self.acts[act]} ms")
            if act != 1:
                continue
            if ats[0] != 0 or ats[-1] != self.acts[1]:
                raise Refused(f"{name}: a wait track runs from 0 to the act's end, {self.acts[1]} ms")
            first, last = keys[0][1], keys[-1][1]
            if (first - last) % turn if what == WHATS.index("turn") else first != last:
                raise Refused(f"{name}: it ends where it did not begin, and the round would show a seam")
            entered = self.tracks.get((0, figure, what))
            left = entered[-1][1] if entered else self.figures[figure]["base"][what]
            if first != left:
                raise Refused(f"{name}: it begins where enter did not leave the figure")

    def pack(self):
        n = self.n
        order = sorted(self.tracks)
        keys = [k for where in order for k in self.tracks[where]]
        body = bytearray()
        for kind, ink, first, count, space in self.paints:
            body += PAINT.pack(kind, ink, first, count, *space)
        for at, ink in self.stops:
            body += STOP.pack(at, ink)
        for f in self.figures:
            body += FIGURE.pack(f["kind"], f["parent"], f["paint"], *f["shape"], *f["pivot"], *f["base"])
        for verb, px, py in self.pieces:
            body += PIECE.pack(verb, px, py)
        first = 0
        for act, figure, what in order:
            body += TRACK.pack(act, figure, what, first, len(self.tracks[(act, figure, what)]))
            first += len(self.tracks[(act, figure, what)])
        for at, value, ease in keys:
            body += KEY.pack(at, value, ease)
        size = HEAD.size + len(body)
        head = HEAD.pack(n["BOX_SCENE_MAGIC"], size, self.units, self.paper, *self.acts, len(self.paints),
                         len(self.stops), len(self.figures), len(self.pieces), len(order), len(keys))
        return head + bytes(body)


def read(path, n):
    data = open(path, "rb").read()
    if len(data) < HEAD.size or data[:8] != n["BOX_SCENE_MAGIC"]:
        print(f"scene: {path} is no scene")
        return 1
    magic, size, units, paper, enter, wait, leave, paints, stops, figures, pieces, tracks, keys = HEAD.unpack_from(data)
    print(f"scene: {size} bytes, {units} units across, paper #{paper:06X}, acts {enter}/{wait}/{leave} ms, "
          f"{paints} paints, {stops} stops, {figures} figures, {pieces} pieces, {tracks} tracks, {keys} keys")
    at = HEAD.size + paints * PAINT.size + stops * STOP.size
    kinds = {n[v]: k for k, v in KINDS.items()}
    for i in range(figures):
        kind, parent, paint, *rest = FIGURE.unpack_from(data, at + i * FIGURE.size)
        group = "-" if parent == n["BOX_SCENE_NONE"] else parent
        print(f"figure {i} {kinds.get(kind, kind)} in {group} paint {paint} shape {rest[0:4]} pivot {rest[4:6]} "
              f"base {rest[6:12]}")
    return 0


def main(argv):
    if len(argv) < 2 or (argv[1], len(argv)) not in (("pack", 4), ("read", 3)):
        sys.exit(__doc__)
    n = numbers()
    if argv[1] == "read":
        return read(argv[2], n)
    scene = Scene(n)
    try:
        scene.parse(argv[2])
    except Refused as why:
        sys.exit(f"scene: {why}")
    packed = scene.pack()
    with open(argv[3], "wb") as out:
        out.write(packed)
    print(f"scene: {argv[3]} packed, {len(packed)} bytes — {len(scene.figures)} figures, {len(scene.tracks)} tracks")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
