#!/usr/bin/env python3
"""draw.py — the boot scenes (src/userspace/scenes/*.txt) drawn from the
owner's drawing, duppogluten.svg: four designs, each dark and light, each
still and alive.

  draw.py DRAWING OUTDIR       make scenes-draw does it for the tree

The designs: Duppo alone (boot), Duppo over the word (duppo-word), the logo in
a line (duppo-line), the word with the spheres in its letters (duppo-letters)
— each the drawing's shapes where it has them (svg.py), every soft light kept
inside the letter it falls on (lights.py).

Still: the logo as drawn, faded into its paper when it leaves. Alive: Duppo
asleep, his eye shut, for as long as the machine takes to reach the display;
then he wakes — his eye opens and blinks once more, turns on his sphere, a hop
squashes him — his moons slip their places into tilted orbits that pass behind
him, the letters hop in a wave and a glint runs over them; round and round
while the machine gets ready; and when it leaves, his moons fall in and the
whole logo is drawn into his pupil — or, with no Duppo, the letters drop.
Nothing is drawn past the logo's reach: a frame of leave costs what a frame of
the logo costs, never a screen's worth of layers.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lights import Lights  # noqa: E402
from svg import MM, Box, Design, EllipseOf, Flatten, Letters, Refused  # noqa: E402

ENTER, WAIT, LEAVE = 1200, 3600, 520
DUPPO = ("body", "socket", "white", "pupil", "medium", "small", "tiny")
# Each design by the drawing's shapes it is made of — numbered from 1 in the
# order librsvg lays them (svg.py Flatten) — and how wide it stands across the
# scene's thousand units; Duppo's own size where he stands alone.
DESIGNS = {
    "boot":          {"duppo": [28, 29, 30, 31, 32, 33, 34], "across": None},
    "duppo-word":    {"duppo": [10, 11, 12, 13, 14, 15, 16], "word": (17, 18), "across": 850.0, "look": (0.02, 0.16)},
    "duppo-line":    {"word": (1, 2), "duppo": [3, 4, 5, 6, 7, 8, 9], "across": 900.0, "look": (0.17, 0.02)},
    "duppo-letters": {"spheres": [19, 20, 21, 22, 23], "word": (24, 25), "across": 850.0},
}
FLAT, DEPTH = 0.5, 0.22                                # how an orbit is seen leaned back, how much nearer grows
ORBITS = {"medium": (1.5, 1), "small": (1.8, 1), "tiny": (1.32, 2)}   # radius in body radii, turns a round


def F2(v):
    return f"{v:.3f}"


class Moves:
    def __init__(self):
        self.lines = []

    def Add(self, fig, act, what, keys):
        self.lines.append(f"move {fig} {act} {what} " + " ".join(
            f"{int(round(t))}:{v:.4f}" + (f":{e}" if e else "") for t, v, e in keys))


def Orbit(cx, cy, px, py, radius, ex, ey):
    """A moon's orbit about cx, cy: a circle of `radius` leaned back, its line
    of nodes the way the moon stands from the middle, its near half passing
    on the side away from the eye at ex, ey — where an angle along it puts
    the moon, and how near (the angle 0 the node, neither near nor far)."""
    ang = math.atan2(py - cy, px - cx)
    ux, uy = math.cos(ang), math.sin(ang)
    vx, vy = -uy, ux
    way = -1 if vx * (ex - cx) + vy * (ey - cy) > 0 else 1

    def at(th):
        a, b = radius * math.cos(th), radius * FLAT * math.sin(th) * way
        return cx + ux * a + vx * b, cy + uy * a + vy * b, math.sin(th)
    return at


def Build(name, spec, shapes, grads, lights, paper, alive):
    pick = lambda i: shapes[i - 1]
    used = [pick(i) for k in ("duppo", "spheres") for i in spec.get(k, [])]
    if "word" in spec:
        used += [pick(i) for i in spec["word"]]
    boxes = [Box(sh["pieces"]) for sh in used]
    x0, y0 = min(b[0] for b in boxes), min(b[1] for b in boxes)
    x1, y1 = max(b[2] for b in boxes), max(b[3] for b in boxes)
    if spec["across"] is None:
        body = EllipseOf(pick(spec["duppo"][0])["pieces"])
        D = Design(300.36 / (2 * body[2]), (body[0], body[1]))
    else:
        D = Design(spec["across"] / (x1 - x0), ((x0 + x1) / 2, (y0 + y1) / 2))
    figs, M = [], Moves()
    reach = [D.Pt(x0, y0), D.Pt(x1, y1)]                # every place a figure goes, for the veil
    place = lambda i: (lambda e: (D.Pt(e[0], e[1]), (e[2] * D.s, e[3] * D.s)))(EllipseOf(pick(i)["pieces"]))
    top = "all" if alive else None
    pupil_at = None
    if alive:
        if "duppo" in spec:
            (px, py), _ = place(spec["duppo"][3])
            pupil_at = (px, py)
            figs.append(f"group all pivot={F2(px)},{F2(py)}")
        else:
            figs.append("group all")

    def Duppo():
        parts = {part: (i, place(i)) for part, i in zip(DUPPO, spec["duppo"])}
        paints = {part: D.Gradient(grads[pick(i)["grad"]]) for part, (i, _) in parts.items()}
        (bx, by), (br, _) = parts["body"][1]
        g = f" in={top}" if top else ""
        figs.append(f"group duppo{g} pivot={F2(bx)},{F2(by + br)}")

        def Disc(part, figname=None, extra=""):
            (cx, cy), (rx, ry) = parts[part][1]
            parent = "eye" if part in ("socket", "white", "pupil") else "duppo"
            figs.append(f"ellipse {figname or part} in={parent} center={F2(cx)},{F2(cy)} radius={F2(rx)},{F2(ry)} "
                        f"paint={paints[part]} pivot={F2(cx)},{F2(cy)}{extra}")

        moons = {}
        (ex, ey), _ = parts["socket"][1]
        for m in ("medium", "small", "tiny"):
            (mx, my), _ = parts[m][1]
            moons[m] = (Orbit(bx, by, mx, my, ORBITS[m][0] * br, ex, ey), True)
        if alive:
            for m in ("medium", "small", "tiny"):
                Disc(m, m + "_back", " opacity=0")
        Disc("body")
        (sx, sy), _ = parts["socket"][1]
        figs.append(f"group eye in=duppo pivot={F2(sx)},{F2(sy)}" + (" tall=0.08" if alive else ""))
        for part in ("socket", "white", "pupil"):
            Disc(part)
        for m in ("medium", "small", "tiny"):
            Disc(m)
        if not alive:
            return
        R = br
        R2 = ORBITS["small"][0] * R * (1 + DEPTH)
        reach.extend([(bx - R2, by - R2), (bx + R2, by + R2)])
        # He wakes: the eye opens, blinks once more; then the hop and the look of enter.
        M.Add("eye", "enter", "tall", [(60, 0.08, ""), (240, 1, "out"), (420, 1, ""), (470, 0.12, "in"),
                                        (570, 1, "out")])
        M.Add("duppo", "enter", "wide", [(300, 1, ""), (400, 1.05, "out"), (520, 0.95, "out"), (760, 1, "inout"),
                                          (820, 1.07, "out"), (960, 1, "inout")])
        M.Add("duppo", "enter", "tall", [(300, 1, ""), (400, 0.93, "out"), (520, 1.06, "out"), (760, 1, "inout"),
                                          (820, 0.92, "out"), (960, 1, "inout")])
        M.Add("duppo", "enter", "y", [(400, by + R, ""), (580, by + R - 0.16 * R, "out"), (760, by + R, "in")])
        lx, ly = spec.get("look", (0.12, -0.05))
        look = [(sx, sy, 1.0), (sx + lx * R, sy + ly * R, 1 - abs(lx) * 0.45),
                (sx - 0.10 * R, sy - 0.04 * R, 1.0), (sx + 0.04 * R, sy + 0.02 * R, 0.99)]
        M.Add("eye", "enter", "x", [(300, look[0][0], ""), (560, look[1][0], "inout"), (880, look[2][0], "inout"),
                                     (1200, look[3][0], "inout")])
        M.Add("eye", "enter", "y", [(300, look[0][1], ""), (560, look[1][1], "inout"), (880, look[2][1], "inout"),
                                     (1200, look[3][1], "inout")])
        M.Add("eye", "enter", "wide", [(300, 1, ""), (560, look[1][2], "inout"), (880, look[2][2], "inout"),
                                        (1200, look[3][2], "inout")])
        (ux, uy), _ = parts["pupil"][1]
        M.Add("pupil", "enter", "x", [(300, ux, ""), (560, ux + lx * 0.22 * R, "inout"), (880, ux - 0.025 * R, "inout"),
                                       (1200, ux + 0.01 * R, "inout")])
        M.Add("pupil", "enter", "y", [(300, uy, ""), (560, uy + ly * 0.22 * R, "inout"), (880, uy - 0.01 * R, "inout"),
                                       (1200, uy + 0.005 * R, "inout")])
        # Wait: a bob, a look about, a blink — beginning and ending where enter left him.
        e = look[3]
        M.Add("duppo", "wait", "y", [(0, by + R, ""), (900, by + R - 0.035 * R, "inout"), (1800, by + R, "inout"),
                                      (2700, by + R - 0.035 * R, "inout"), (3600, by + R, "inout")])
        M.Add("eye", "wait", "x", [(0, e[0], ""), (700, e[0], ""), (1100, sx + lx * 0.8 * R, "inout"),
                                    (2300, sx + lx * 0.8 * R, ""), (2800, sx - 0.08 * R, "inout"),
                                    (3600, e[0], "inout")])
        M.Add("eye", "wait", "y", [(0, e[1], ""), (700, e[1], ""), (1100, sy + ly * 0.8 * R, "inout"),
                                    (2300, sy + ly * 0.8 * R, ""), (2800, sy - 0.03 * R, "inout"),
                                    (3600, e[1], "inout")])
        M.Add("eye", "wait", "wide", [(0, e[2], ""), (700, e[2], ""), (1100, 1 - abs(lx) * 0.36, "inout"),
                                       (2300, 1 - abs(lx) * 0.36, ""), (2800, 1.0, "inout"), (3600, e[2], "inout")])
        M.Add("eye", "wait", "tall", [(0, 1, ""), (1650, 1, ""), (1710, 0.08, "in"), (1810, 1, "out"), (3600, 1, "")])
        # The moons: from their places into their orbits, speeding up to the
        # speed they keep round and round, then whole turns.
        for m in ("medium", "small", "tiny"):
            at, _ = moons[m]
            omega = 2 * math.pi * ORBITS[m][1] / WAIT
            t0 = 260
            def angle_enter(t):
                return 0.0 if t <= t0 else omega * (t - t0) ** 2 / (2 * (ENTER - t0))
            def onto(t):                      # how far from its place onto its orbit
                w = min(max((t - t0) / (ENTER - t0), 0.0), 1.0)
                return w * w * (3 - 2 * w)
            entered = angle_enter(ENTER)
            (mx, my), _ = parts[m][1]
            steps = list(range(t0, ENTER, 40)) + [ENTER]
            for fig, show_front in ((m, True), (m + "_back", False)):
                xs, ys, ss, os_ = [], [], [], []
                was = None
                for t in [0] + steps:
                    ox, oy, z = at(angle_enter(t))
                    w = onto(t)
                    x, y = mx + (ox - mx) * w, my + (oy - my) * w
                    xs.append((t, x, "")); ys.append((t, y, "")); ss.append((t, 1 + DEPTH * z * w, ""))
                    vis = 255 if (z >= 0) == show_front else 0
                    if vis != was:
                        os_.append((t, vis, "step" if t else "")); was = vis
                M.Add(fig, "enter", "x", xs); M.Add(fig, "enter", "y", ys); M.Add(fig, "enter", "scale", ss)
                M.Add(fig, "enter", "opacity", os_ if len(os_) > 1 else os_ + [(ENTER, os_[0][1], "")])
                xs, ys, ss, os_ = [], [], [], []
                was = None
                for t in range(0, WAIT + 1, 40):
                    x, y, z = at(entered + omega * t)
                    xs.append((t, x, "")); ys.append((t, y, "")); ss.append((t, 1 + DEPTH * z, ""))
                    vis = 255 if (z >= 0) == show_front else 0
                    if vis != was:
                        os_.append((t, vis, "step" if t else "")); was = vis
                if os_[-1][0] != WAIT:
                    os_.append((WAIT, os_[-1][1], ""))
                M.Add(fig, "wait", "x", xs); M.Add(fig, "wait", "y", ys); M.Add(fig, "wait", "scale", ss)
                M.Add(fig, "wait", "opacity", os_)
        # Leave: the eye home, then everything into the pupil.
        M.Add("eye", "leave", "x", [(90, sx, "out")]); M.Add("eye", "leave", "y", [(90, sy, "out")])
        M.Add("eye", "leave", "wide", [(90, 1, "out")]); M.Add("eye", "leave", "tall", [(90, 1, "out")])
        M.Add("pupil", "leave", "x", [(90, ux, "out")]); M.Add("pupil", "leave", "y", [(90, uy, "out")])
        M.Add("duppo", "leave", "y", [(90, by + R, "out")])
        for m in ("medium", "small", "tiny"):
            for fig in (m, m + "_back"):
                M.Add(fig, "leave", "x", [(230, bx, "in")]); M.Add(fig, "leave", "y", [(230, by, "in")])
                M.Add(fig, "leave", "scale", [(230, 0.4, "in")])
                M.Add(fig, "leave", "opacity", [(230, 0, "in")])

    def Sphere(k, i):
        """The sphere in letter k's hole, under the letter, moving with it."""
        p = D.Gradient(grads[pick(i)["grad"]])
        (cx, cy), (rx, ry) = place(i)
        figs.append(f"ellipse sphere{k} in=letter{k} center={F2(cx)},{F2(cy)} radius={F2(rx)},{F2(ry)} paint={p} "
                    f"pivot={F2(cx)},{F2(cy)}")
        if alive:
            t = 200 + 90 * k
            M.Add(f"sphere{k}", "enter", "scale", [(t, 1, ""), (t + 150, 1.24, "out"), (t + 380, 1, "inout")])
            w = 900 + 110 * k
            M.Add(f"sphere{k}", "wait", "scale", [(0, 1, ""), (w, 1, ""), (w + 160, 1.14, "out"),
                                                   (w + 400, 1, "inout"), (WAIT, 1, "")])
            M.Add(f"sphere{k}", "leave", "scale", [(40 * k, 1, ""), (40 * k + 110, 1.25, "out"),
                                                    (40 * k + 260, 0, "in")])

    def Word():
        dark, light = (pick(i) for i in spec["word"])
        pd, pl = D.Gradient(grads[dark["grad"]]), D.Gradient(grads[light["grad"]])
        g = f" in={top}" if top else ""
        figs.append(f"group word{g}")
        ld, ll = Letters(dark["pieces"]), Letters(light["pieces"])
        base = max(Box(b)[3] for b in ll[-1:])                 # the baseline: the o's foot
        wx0, wy0, wx1, wy1 = Box(light["pieces"])
        height = (base - wy0) * D.s
        (_bx, _by) = D.Pt(wx1, base)
        reach.append((_bx, _by + 0.6 * height))
        for k, (a, b) in enumerate(zip(ld, ll)):
            lx0, ly0, lx1, ly1 = Box(b)
            (px, py) = D.Pt((lx0 + lx1) / 2, base)
            figs.append(f"group letter{k} in=word pivot={F2(px)},{F2(py)}")
            if "spheres" in spec:
                Sphere(k, sorted(spec["spheres"], key=lambda i: EllipseOf(pick(i)["pieces"])[0])[k])
            figs.append(f"path edge{k} in=letter{k} paint={pd} d={D.Data(a)}")
            figs.append(f"path face{k} in=letter{k} paint={pl} d={D.Data(b)}")
        for blob in lights:
            bx, by = blob["unit"][4] * MM, blob["unit"][5] * MM
            if not (wx0 - 20 <= bx <= wx1 + 20 and wy0 - 20 <= by <= wy1 + 20):
                continue
            p, (cx, cy, rx, ry) = D.Light(blob)
            for k, b in enumerate(ll):
                lx0, ly0, lx1, ly1 = Box(b)
                if cx + rx > lx0 and cx - rx < lx1 and cy + ry > ly0 and cy - ry < ly1:
                    figs.append(f"path {blob['id']}_{k} in=letter{k} shape=face{k} paint={p}")
        if not alive:
            return
        for k, b in enumerate(ll):
            lx0, ly0, lx1, ly1 = Box(b)
            # A glint: the letter's face once more, a soft white light across its top.
            gx, gy = D.Pt((lx0 + lx1) / 2, ly0 + (ly1 - ly0) * 0.3)
            r = (lx1 - lx0) * D.s * 0.62
            name = D.Name()
            D.lines.append(f"paint {name} radial matrix={F2(r)},0,0,{F2(r * 0.7)},{F2(gx)},{F2(gy)} center=0,0 radius=1 "
                           "stops=0:#73FFFFFF,0.45:#38FFFFFF,1:#00FFFFFF")
            figs.append(f"path glint{k} in=letter{k} shape=face{k} paint={name} opacity=0")
            (px, py) = D.Pt((lx0 + lx1) / 2, base)
            t = 420 + 80 * k
            M.Add(f"letter{k}", "enter", "y", [(t, py, ""), (t + 150, py - 0.13 * height, "out"), (t + 290, py, "in")])
            M.Add(f"letter{k}", "enter", "tall", [(t + 290, 1, ""), (t + 340, 0.9, "out"), (t + 460, 1, "inout")])
            M.Add(f"letter{k}", "enter", "wide", [(t + 290, 1, ""), (t + 340, 1.06, "out"), (t + 460, 1, "inout")])
            gt = 640 + 60 * k
            M.Add(f"glint{k}", "enter", "opacity", [(gt, 0, ""), (gt + 120, 255, "out"), (gt + 300, 0, "in")])
            wt = 1500 + 70 * k
            M.Add(f"glint{k}", "wait", "opacity", [(0, 0, ""), (wt, 0, ""), (wt + 160, 200, "out"),
                                                   (wt + 420, 0, "in"), (WAIT, 0, "")])
            if "duppo" not in spec:
                M.Add(f"letter{k}", "leave", "y", [(40 * k, py, ""), (40 * k + 300, py + 0.5 * height, "in")])
                M.Add(f"letter{k}", "leave", "opacity", [(40 * k, 255, ""), (40 * k + 300, 0, "in")])

    order = {"boot": [Duppo], "duppo-word": [Duppo, Word], "duppo-line": [Word, Duppo],
             "duppo-letters": [Word]}[name]
    for step in order:
        step()
    if alive and pupil_at:
        # Drawn into his pupil: a start of surprise, then smaller and smaller
        # about it, gone before the act ends.
        M.Add("all", "leave", "scale", [(90, 1, ""), (170, 1.05, "out"), (450, 0.02, "in")])
        M.Add("all", "leave", "opacity", [(340, 255, ""), (450, 0, "in")])
    else:
        # The logo's reach, every place its figures go, faded into the paper.
        rx0, ry0 = min(p[0] for p in reach) - 12, min(p[1] for p in reach) - 12
        rx1, ry1 = max(p[0] for p in reach) + 12, max(p[1] for p in reach) + 12
        veil = D.Solid(0xFF000000 | int(paper[1:], 16))
        figs.append(f"rect veil box={F2(rx0)},{F2(ry0)},{F2(rx1 - rx0)},{F2(ry1 - ry0)} paint={veil} opacity=0")
        M.Add("veil", "leave", "opacity", [(260, 0, ""), (LEAVE, 255, "inout")] if alive else
                                          [(0, 0, ""), (LEAVE, 255, "inout")])
    kind = "alive" if alive else "still"
    head = [f"# {name}, {kind}: duppogluten.svg as drawn — the letters Gluten's outlines, each light kept",
            "# inside its letter" + (", and Duppo awake" if alive else ""),
            "# Made by tools/scenedraw/draw.py (make scenes-draw), not by hand.",
            f"scene units=1000 paper={paper} enter={ENTER} wait={WAIT} leave={LEAVE}"]
    return "\n".join(head + D.lines + figs + M.lines) + "\n"


def main(argv):
    if len(argv) != 3:
        print("usage: draw.py DRAWING OUTDIR", file=sys.stderr)
        return 2
    drawing, out = argv[1], argv[2]
    try:
        shapes, grads = Flatten(drawing)
    except Refused as e:
        print(f"draw.py: {e}", file=sys.stderr)
        return 1
    lights = Lights(drawing)
    os.makedirs(out, exist_ok=True)
    for name, spec in DESIGNS.items():
        for tone, paper in (("dark", "#000000"), ("light", "#FFFFFF")):
            for alive in (False, True):
                text = Build(name, spec, shapes, grads, lights, paper, alive)
                with open(os.path.join(out, f"{name}-{tone}-{'alive' if alive else 'still'}.txt"), "w") as f:
                    f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
