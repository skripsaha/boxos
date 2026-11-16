"""lights.py — the drawing's soft lights: every ellipse it blurs with a Gaussian
filter, read from the drawing itself.

librsvg lays a blurred shape flat as a picture, which a scene cannot carry; so
each light is taken from the drawing's own markup instead — the ellipse
(Inkscape's sodipodi arc) through every transform above it, in millimetres as
the unit circle mapped by `unit`, its opacity, and the blur's sigma in the same
millimetres. svg.py makes a radial paint of each, and draw.py lays it inside
the letter it falls on.
"""

import math
import re
import xml.etree.ElementTree as ET

SODIPODI = "{http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd}"


def Then(a, b):
    """The affine `b` then `a`, each (A, B, C, D, E, F) as SVG writes one."""
    return (a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1], a[0] * b[2] + a[2] * b[3],
            a[1] * b[2] + a[3] * b[3], a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5])


def Transform(text):
    m = (1, 0, 0, 1, 0, 0)
    for name, args in re.findall(r"(\w+)\s*\(([^)]*)\)", text or ""):
        v = [float(x) for x in re.split(r"[\s,]+", args.strip()) if x]
        if name == "matrix":
            n = tuple(v)
        elif name == "translate":
            n = (1, 0, 0, 1, v[0], v[1] if len(v) > 1 else 0)
        elif name == "scale":
            n = (v[0], 0, 0, v[1] if len(v) > 1 else v[0], 0, 0)
        elif name == "rotate":
            a = math.radians(v[0])
            n = (math.cos(a), math.sin(a), -math.sin(a), math.cos(a), 0, 0)
            if len(v) == 3:
                n = Then(Then((1, 0, 0, 1, v[1], v[2]), n), (1, 0, 0, 1, -v[1], -v[2]))
        else:
            raise ValueError(f"the drawing has a transform this reader does not take: {name}")
        m = Then(m, n)
    return m


def Style(e):
    out = {}
    for item in (e.get("style") or "").split(";"):
        k, _, v = item.partition(":")
        if k.strip():
            out[k.strip()] = v.strip()
    for k in ("fill", "fill-opacity", "opacity", "filter"):
        if e.get(k) is not None and k not in out:
            out[k] = e.get(k)
    return out


def Lights(drawing):
    """Every blurred ellipse of the drawing: {id, unit, sigma, alpha}."""
    root = ET.parse(drawing).getroot()
    tag = lambda e: e.tag.split("}")[1]
    blur = {}
    for f in root.iter():
        if tag(f) == "filter":
            for c in f:
                if tag(c) == "feGaussianBlur":
                    blur[f.get("id")] = float(c.get("stdDeviation"))
    out = []

    def Walk(e, m, opacity):
        st = Style(e)
        m = Then(m, Transform(e.get("transform")))
        opacity *= float(st.get("opacity", "1"))
        if tag(e) == "path" and e.get(SODIPODI + "type") == "arc" and "filter" in st:
            fid = re.match(r"url\(#([^)]+)\)", st["filter"]).group(1)
            cx, cy = float(e.get(SODIPODI + "cx")), float(e.get(SODIPODI + "cy"))
            rx, ry = float(e.get(SODIPODI + "rx")), float(e.get(SODIPODI + "ry"))
            out.append({"id": e.get("id"), "unit": Then(m, (rx, 0, 0, ry, cx, cy)),
                        "sigma": blur[fid] * math.sqrt(abs(m[0] * m[3] - m[1] * m[2])),
                        "alpha": opacity * float(st.get("fill-opacity", "1"))})
        for c in e:
            if tag(c) not in ("defs", "namedview", "metadata"):
                Walk(c, m, opacity)

    Walk(root, (1, 0, 0, 1, 0, 0), 1.0)
    return out
