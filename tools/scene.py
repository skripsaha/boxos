#!/usr/bin/env python3
"""scene.py — a scene (src/include/boxos_scene.h) packed from its text, and
what a packed one holds.

  scene.py pack SOURCE OUT   the text of a scene (src/userspace/scenes/) packed
                             into the form Gangway and the display play
  scene.py read FILE         what a packed scene holds, a line each

The numbers of the form are read out of boxos_scene.h and boxos_brush.h, its
records held to the sizes boxos_scene.h asserts; Duppo's discs, shades and
hues are read out of boxos_mascot.h by name. Nothing here is a second copy of
what the loader, the display or the utility duppo read.

The text — a statement a line, a remark from a # on its own to the end of the
line; places in the scene's units, colours #RRGGBB or #AARRGGBB, numbers as
they are:

  scene units=U paper=#RRGGBB enter=MS wait=MS leave=MS
  paint NAME solid COLOUR
  paint NAME radial shade=SHADE stops=STOPS             Duppo's, by name
  paint NAME radial matrix=A,B,C,D,E,F center=X,Y radius=R stops=AT:COLOUR,...
  paint NAME linear from=X,Y to=X,Y stops=AT:COLOUR,...
  group NAME [FIGURE...]
  ellipse NAME (disc=DISC | oval=OVAL | center=X,Y radius=R[,R]) paint=NAME [FIGURE...]
  rect NAME box=X,Y,W,H paint=NAME [FIGURE...]
  move NAME ACT WHAT MS:VALUE[:EASE] ...

  FIGURE: in=GROUP pivot=X,Y|DISC at=X,Y scale=S wide=S tall=S turn=TURNS opacity=0..255
  ACT: enter wait leave; WHAT: x y (where the pivot is put) wide tall scale turn opacity;
  EASE: linear in out inout step.

A paint's places are its figure's own. A figure's pivot is in its own places
and stays where it is unless at= puts it elsewhere in its group. The wait
act's tracks must begin where enter left the figure and come back to where
they began — a turn by whole turns — so its round has no seam.
"""

import os
import re
import struct
import sys

INCLUDE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "include")

HEAD = struct.Struct("<8s11I")
PAINT = struct.Struct("<4I6i")
STOP = struct.Struct("<2I")
FIGURE = struct.Struct("<3I4i2i6i")
TRACK = struct.Struct("<5I")
KEY = struct.Struct("<IiI")
RECORDS = {"BoxSceneHead": HEAD, "BoxScenePaint": PAINT, "BoxBrushStop": STOP, "BoxSceneFigure": FIGURE,
           "BoxSceneTrack": TRACK, "BoxSceneKey": KEY}

ACTS = ("enter", "wait", "leave")
WHATS = ("x", "y", "wide", "tall", "turn", "opacity")
EASES = ("linear", "in", "out", "inout", "step")
KINDS = {"group": "BOX_SCENE_GROUP", "ellipse": "BOX_SCENE_ELLIPSE", "rect": "BOX_SCENE_RECT"}


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


def mascot():
    """Duppo's numbers in boxos_mascot.h, by name: a record a tuple of its
    fields, an array a list of them; its hues' names and defines."""
    text = re.sub(r"/\*.*?\*/", "", open(os.path.join(INCLUDE, "boxos_mascot.h")).read(), flags=re.S)
    names = {}
    for name, value in re.findall(r"^#define\s+(BOXOS_\w+)\s+(0x[0-9A-Fa-f]+|\d+)u?\s*$", text, re.M):
        names[name] = int(value, 0)
    enum = re.search(r"enum\s*\{([^}]*)\}", text)
    for index, name in enumerate(n.strip() for n in enum.group(1).split(",") if n.strip()):
        names[name] = index

    def value(token):
        token = token.strip()
        if token in names:
            return names[token]
        if re.fullmatch(r"0x[0-9A-Fa-f]+u?|\d+u?", token):
            return int(token.rstrip("u"), 0)
        return float(token.rstrip("f"))

    def braces(body):
        body = body.strip()
        if not body.startswith("{"):
            return value(body)
        items, depth, start = [], 0, 1
        for i, c in enumerate(body):
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            if (c == "," and depth == 1) or (c == "}" and depth == 0):
                if body[start:i].strip():
                    items.append(braces(body[start:i]))
                start = i + 1
        return tuple(items)

    for name, array, body in re.findall(r"BOXOS_MASCOT_NUMBER\s+\w+\s+(\w+)(\[\w*\])?\s*=\s*(\{.*?\});", text, re.S):
        got = braces(body)
        names[name] = list(got) if array else got
    return names


def refer(m, token):
    """A mascot number named in the text, NAME or NAME[I]."""
    found = re.fullmatch(r"(\w+)(?:\[(\d+)\])?", token)
    if not found or found.group(1) not in m:
        raise Refused(f"boxos_mascot.h names no {token}")
    got = m[found.group(1)]
    if found.group(2) is not None:
        if not isinstance(got, list) or int(found.group(2)) >= len(got):
            raise Refused(f"{found.group(1)} has no [{found.group(2)}]")
        got = got[int(found.group(2))]
    return got


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


def mix(under, over, weight):
    """Two colours mixed straight, as BoxLib's color_mix and Painter's Color::Mix mix them."""
    out = 0
    for shift in (0, 8, 16):
        a, b = (under >> shift) & 0xFF, (over >> shift) & 0xFF
        out |= ((a * (255 - weight) + b * weight) // 255) << shift
    return out


class Scene:
    def __init__(self, n, m):
        self.n, self.m = n, m
        self.units = None
        self.paper = 0
        self.acts = [0, 0, 0]
        self.paints, self.paint_names, self.stops = [], {}, []
        self.figures, self.figure_names = [], {}
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

    def mascot_stops(self, stops):
        hues, m = self.m["BoxDuppoHues"], self.m
        out = []
        for at, hue in stops:
            if hue == m["BOXOS_DUPPO_GLOW"]:
                rgb = mix(hues[m["BOXOS_DUPPO_CORE"]], 0xFFFFFF, m["BOXOS_DUPPO_GLOW_SHARE"])
            elif hue == m["BOXOS_DUPPO_PUPIL_EDGE"]:
                rgb = mix(hues[m["BOXOS_DUPPO_CORE"]], 0xFFFFFF, m["BOXOS_DUPPO_RIM_SHARE"])
            else:
                rgb = hues[hue]
            out.append((at, 0xFF000000 | rgb))
        return out

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
        elif kind == "radial" and rest and rest[0].startswith("shade="):
            o = self.options(rest, ("shade", "stops"))
            shade = refer(self.m, o["shade"])
            paint = (n["BOX_BRUSH_PAINT_RADIAL"], 0, self.mascot_stops(refer(self.m, o["stops"])),
                     self.shade_space(*shade))
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
        shape_keys = {"group": (), "ellipse": ("disc", "oval", "center", "radius", "paint"),
                      "rect": ("box", "paint")}[kind]
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
            given = [k for k in ("disc", "oval", "center") if k in o]
            if len(given) != 1:
                raise Refused(f"ellipse {name} says one of disc, oval or center")
            if "disc" in o:
                x, y, r = refer(self.m, o["disc"])
                shape = [x, y, r, r]
            elif "oval" in o:
                shape = list(refer(self.m, o["oval"]))
            else:
                radius = floats(o["radius"], 2 if "," in o.get("radius", "") else 1) if "radius" in o else None
                if radius is None:
                    raise Refused(f"ellipse {name} says its radius")
                shape = floats(o["center"], 2) + (radius * 2 if len(radius) == 1 else radius)
            shape = [self.place(v) for v in shape]
        elif kind == "rect":
            if "box" not in o:
                raise Refused(f"rect {name} says its box")
            shape = [self.place(v) for v in floats(o["box"], 4)]
        if kind != "group":
            if o.get("paint") not in self.paint_names:
                raise Refused(f"{kind} {name} is painted with {o.get('paint')}, which is no paint said before it")
            paint = self.paint_names[o["paint"]]
        if "pivot" in o:
            pivot = refer(self.m, o["pivot"])[:2] if re.match(r"^[A-Za-z]", o["pivot"]) else floats(o["pivot"], 2)
        else:
            pivot = [0.0, 0.0]
        pivot = [self.place(v) for v in pivot]
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
        first = 0
        for act, figure, what in order:
            body += TRACK.pack(act, figure, what, first, len(self.tracks[(act, figure, what)]))
            first += len(self.tracks[(act, figure, what)])
        for at, value, ease in keys:
            body += KEY.pack(at, value, ease)
        size = HEAD.size + len(body)
        head = HEAD.pack(n["BOX_SCENE_MAGIC"], size, self.units, self.paper, *self.acts, len(self.paints),
                         len(self.stops), len(self.figures), len(order), len(keys))
        return head + bytes(body)


def read(path, n):
    data = open(path, "rb").read()
    if len(data) < HEAD.size or data[:8] != n["BOX_SCENE_MAGIC"]:
        print(f"scene: {path} is no scene")
        return 1
    magic, size, units, paper, enter, wait, leave, paints, stops, figures, tracks, keys = HEAD.unpack_from(data)
    print(f"scene: {size} bytes, {units} units across, paper #{paper:06X}, acts {enter}/{wait}/{leave} ms, "
          f"{paints} paints, {stops} stops, {figures} figures, {tracks} tracks, {keys} keys")
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
    scene = Scene(n, mascot())
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
