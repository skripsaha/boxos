#!/usr/bin/env python3
"""gen_dither_noise.py — src/userspace/painter/src/core/dither_noise.h, the
pattern a canvas's light is written to the glass with (Canvas::CopyOut,
Dither::On): 64 x 64 thresholds, 0 to 255, each sixteen times, laid as blue
noise — no two near pixels alike, no pattern the eye picks out.

    python3 tools/gen_dither_noise.py > src/userspace/painter/src/core/dither_noise.h

Ulichney's void and cluster: a few points spread until no cluster is tighter
than the widest void, then every pixel ranked by where it would go next, each
rank taken from the tightest cluster or the widest void of what is placed, on
a torus so the tile repeats with no seam. Not a build dependency: the header
is checked in; the draw stand holds it to its counts.
"""
import math
import random

SIZE = 64
SIGMA = 1.5
CELLS = SIZE * SIZE


def kernel():
    near = {}
    reach = int(math.ceil(SIGMA * 4))
    for dy in range(-reach, reach + 1):
        for dx in range(-reach, reach + 1):
            near[(dx, dy)] = math.exp(-(dx * dx + dy * dy) / (2 * SIGMA * SIGMA))
    return list(near.items())


KERNEL = kernel()


def spread(energy, index, sign):
    x, y = index % SIZE, index // SIZE
    for (dx, dy), weight in KERNEL:
        energy[((y + dy) % SIZE) * SIZE + (x + dx) % SIZE] += sign * weight


def tightest(points, energy):
    return max((i for i in range(CELLS) if points[i]), key=lambda i: energy[i])


def widest(points, energy):
    return min((i for i in range(CELLS) if not points[i]), key=lambda i: energy[i])


def main():
    rng = random.Random(1907)
    points = [False] * CELLS
    energy = [0.0] * CELLS
    first = CELLS // 10
    for i in rng.sample(range(CELLS), first):
        points[i] = True
        spread(energy, i, 1)
    while True:
        cluster = tightest(points, energy)
        points[cluster] = False
        spread(energy, cluster, -1)
        void = widest(points, energy)
        if void == cluster:
            points[cluster] = True
            spread(energy, cluster, 1)
            break
        points[void] = True
        spread(energy, void, 1)
    rank = [0] * CELLS
    placed, placed_energy = points[:], energy[:]
    for r in range(first - 1, -1, -1):
        cluster = tightest(placed, placed_energy)
        placed[cluster] = False
        spread(placed_energy, cluster, -1)
        rank[cluster] = r
    for r in range(first, CELLS):
        void = widest(points, energy)
        points[void] = True
        spread(energy, void, 1)
        rank[void] = r
    thresholds = [r * 256 // CELLS for r in rank]
    rows = []
    for y in range(SIZE):
        line = thresholds[y * SIZE:(y + 1) * SIZE]
        rows.append("    {" + ",".join("%3d" % v for v in line[:32]) + ",")
        rows.append("     " + ",".join("%3d" % v for v in line[32:]) + "},")
    print("\n".join([
        "#ifndef PAINTER_CORE_DITHER_NOISE_H",
        "#define PAINTER_CORE_DITHER_NOISE_H",
        "",
        "/* dither_noise.h — the thresholds a canvas's light is written to the glass",
        " * with: 64 x 64, 0 to 255, each sixteen times, laid as blue noise. GENERATED",
        " * by tools/gen_dither_noise.py; not edited by hand. */",
        "",
        "#include <cstdint>",
        "",
        "namespace painter::core::light {",
        "",
        "inline constexpr std::uint8_t DitherNoise[64][64] = {",
        *rows,
        "};",
        "",
        "}  // namespace painter::core::light",
        "",
        "#endif  // PAINTER_CORE_DITHER_NOISE_H",
    ]))


if __name__ == "__main__":
    main()
