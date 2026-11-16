"""svg.py — the drawing as shapes: duppogluten.svg flattened by librsvg into
paths and radial gradients, and the places of one design in a scene's units.

librsvg (`rsvg-convert -f svg`) lays every shape of the drawing and every
letter of its words as outlines, in the order drawn, the gradients in its own
pixels. The letters are Gluten's, so Gluten must be a font fontconfig knows:
without it librsvg lays the words in another face and says nothing, and the
scenes would be drawn in letters that are not the drawing's. That is refused
before anything is flattened.
"""

import math
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from scene import outlines  # noqa: E402

MM = 96 / 25.4   # the drawing's pixels a millimetre
FONT = "Gluten"


class Refused(Exception):
    pass


def Tag(e):
    return e.tag.split("}")[1]


def Rgba(color, opacity):
    """librsvg's rgb(R%, G%, B%) and an opacity as 0xAARRGGBB."""
    m = re.fullmatch(r"rgb\(([\d.]+)%,\s*([\d.]+)%,\s*([\d.]+)%\)", color.strip())
    if not m:
        raise Refused(f"a colour librsvg wrote is not rgb(%): {color}")
    r, g, b = (round(float(v) * 2.55) for v in m.groups())
    return (round(float(opacity) * 255) << 24) | (r << 16) | (g << 8) | b


def Flatten(drawing):
    """The drawing laid flat by librsvg: its shapes in the order drawn, each
    {pieces, grad, color}, and its radial gradients by id — (matrix, cx, cy,
    r, stops)."""
    try:
        fonts = subprocess.run(["fc-list", FONT], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        raise Refused(f"fontconfig could not be asked for {FONT}: {e}")
    if FONT not in fonts:
        raise Refused(f"{FONT} is no font fontconfig knows; librsvg would lay the drawing's words in "
                      f"another face. Install Gluten (Google Fonts, OFL) and draw again")
    with tempfile.TemporaryDirectory() as work:
        flat = os.path.join(work, "flat.svg")
        try:
            subprocess.run(["rsvg-convert", "-f", "svg", "-o", flat, drawing], check=True)
        except (OSError, subprocess.CalledProcessError) as e:
            raise Refused(f"rsvg-convert could not lay {drawing} flat: {e}")
        root = ET.parse(flat).getroot()
    grads, shapes = {}, []
    for e in root.iter():
        if Tag(e) == "radialGradient":
            t = [float(v) for v in re.findall(r"[-\d.e]+", e.get("gradientTransform", "matrix(1,0,0,1,0,0)"))]
            stops = [(float(s.get("offset")), Rgba(s.get("stop-color"), s.get("stop-opacity", "1")))
                     for s in e if Tag(s) == "stop"]
            grads[e.get("id")] = (t, float(e.get("cx")), float(e.get("cy")), float(e.get("r")), stops)
    for e in root.iter():
        if Tag(e) == "path":
            fill = e.get("fill")
            m = re.fullmatch(r"url\(#(.+)\)", fill or "")
            shapes.append({"pieces": outlines(e.get("d")), "grad": m.group(1) if m else None,
                           "color": None if m else Rgba(fill, e.get("fill-opacity", "1"))})
    return shapes, grads


def Box(pieces):
    xs, ys = [p[1] for p in pieces], [p[2] for p in pieces]
    return min(xs), min(ys), max(xs), max(ys)


def EllipseOf(pieces):
    """The ellipse a shape drawn as one is: its middle and its radii."""
    x0, y0, x1, y1 = Box(pieces)
    return (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2


def Contours(pieces):
    out, now = [], []
    for p in pieces:
        if p[0] == "MOVE" and now:
            out.append(now)
            now = []
        now.append(p)
    if now:
        out.append(now)
    return out


def Letters(pieces):
    """A word's contours grouped into its letters, left to right: a contour
    whose box lies in a larger one's is that letter's hole."""
    cs = Contours(pieces)
    boxes = [Box(c) for c in cs]
    owner = list(range(len(cs)))
    for i, a in enumerate(boxes):
        for j, b in enumerate(boxes):
            if i != j and b[0] <= a[0] and b[1] <= a[1] and a[2] <= b[2] and a[3] <= b[3] and \
               (b[2] - b[0]) * (b[3] - b[1]) > (a[2] - a[0]) * (a[3] - a[1]):
                owner[i] = j
    groups = {}
    for i, o in enumerate(owner):
        while owner[o] != o:
            o = owner[o]
        groups.setdefault(o, []).append(i)
    ordered = sorted(groups.values(),
                     key=lambda g: boxes[g[0]][0] if owner[g[0]] == g[0] else min(boxes[i][0] for i in g))
    return [[p for i in sorted(g, key=lambda i: (owner[i] != i, i)) for p in cs[i]] for g in ordered]


class Design:
    """One design's places: the drawing's pixels onto the scene's units —
    `scale` units a pixel, `origin` the pixel at the scene's 0,0 — and the
    paints it says, in the scene's text, as it goes."""

    def __init__(self, scale, origin):
        self.s, self.o = scale, origin
        self.lines, self.paints = [], 0

    def Pt(self, x, y):
        return (x - self.o[0]) * self.s, (y - self.o[1]) * self.s

    def Data(self, pieces):
        """Pieces as a path's data in the scene's units, every contour closed."""
        out, i = [], 0
        while i < len(pieces):
            verb = pieces[i][0]
            if verb == "MOVE":
                if out:
                    out.append("Z")
                out.append("M %.2f %.2f" % self.Pt(*pieces[i][1:]))
                i += 1
            elif verb == "LINE":
                out.append("L %.2f %.2f" % self.Pt(*pieces[i][1:]))
                i += 1
            elif verb == "QUAD":
                out.append("Q %.2f %.2f %.2f %.2f" % (self.Pt(*pieces[i][1:]) + self.Pt(*pieces[i + 1][1:])))
                i += 2
            else:
                out.append("C %.2f %.2f %.2f %.2f %.2f %.2f" % (self.Pt(*pieces[i][1:]) + self.Pt(*pieces[i + 1][1:]) +
                                                                self.Pt(*pieces[i + 2][1:])))
                i += 3
        out.append("Z")
        return " ".join(out)

    def Name(self):
        name = f"p{self.paints}"
        self.paints += 1
        return name

    def Radial(self, matrix, center, radius, stops):
        """A radial paint said: `matrix` in the drawing's pixels, `center` and
        `radius` in the gradient's own places as the text says them, its stops
        (at, 0xAARRGGBB)."""
        a, b, c, d, e, f = matrix
        s, (ox, oy) = self.s, self.o
        name = self.Name()
        st = ",".join(f"{at:.4f}:#{ink:08X}" for at, ink in stops)
        self.lines.append(f"paint {name} radial matrix={s*a:.6f},{s*b:.6f},{s*c:.6f},{s*d:.6f},{s*(e-ox):.4f},"
                          f"{s*(f-oy):.4f} center={center} radius={radius} stops={st}")
        return name

    def Gradient(self, grad):
        t, cx, cy, r, stops = grad
        return self.Radial(t, f"{cx:.4f},{cy:.4f}", f"{r:.4f}", stops)

    def Solid(self, ink):
        name = self.Name()
        self.lines.append(f"paint {name} solid #{ink:08X}")
        return name

    def Light(self, light):
        """A blurred ellipse (lights.py) as a radial paint: its alpha the
        ellipse's coverage under the Gaussian blur, out to three sigmas past its
        edge; and the box it reaches, in the drawing's pixels."""
        u, sigma, alpha = light["unit"], light["sigma"], light["alpha"]
        rx, ry = math.hypot(u[0], u[1]), math.hypot(u[2], u[3])
        rm = math.sqrt(rx * ry)
        reach = (rm + 3 * sigma) / rm
        stops = []
        for i in range(13):
            at = i / 12
            cover = 0.5 * math.erfc((at * reach * rm - rm) / (sigma * math.sqrt(2)))
            stops.append((at, (round(alpha * cover * 255) << 24) | 0xFEFEFE))
        m = (u[0] * reach * MM, u[1] * reach * MM, u[2] * reach * MM, u[3] * reach * MM, u[4] * MM, u[5] * MM)
        return self.Radial(m, "0,0", "1", stops), (u[4] * MM, u[5] * MM, rx * reach * MM, ry * reach * MM)
