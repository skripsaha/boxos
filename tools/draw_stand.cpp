/*
 * draw stand — the drawing of BoxOS on the host: the shapes, strokes and
 * paints every hand draws with (src/include/boxos_shape.h, boxos_stroke.h,
 * boxos_paint.h) and Painter's engine (src/userspace/painter, painter/core),
 * the very sources the system builds, held to what they must do.
 *
 *     draw_stand checks
 *     draw_stand speed
 *     draw_stand sheet OUT.ppm
 *
 * checks: a shape swept row by row the very cells of every edge laid alone;
 * EVENODD's holes; rounded rectangles of four radii, shrunk as CSS shrinks
 * them; strokes held to their exact areas — every cap, every join, the miter
 * limit, dashes and dots, closed contours, a pen stretched and mirrored;
 * turns held to the host's atan2; gradients — linear, radial from its focal
 * point, conic — past their ends as each Extend says; pictures sharp and
 * smooth, mixed as light. Every check says "held …" or "FAILED …"; the last
 * line counts them.
 */

#include "draw_stand.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace stand {

void Tally::Check(bool Holds, const std::string &What)
{
    if (Holds) {
        Held++;
        std::printf("held %s\n", What.c_str());
    } else {
        Failed++;
        std::printf("FAILED %s\n", What.c_str());
    }
}

static std::uint32_t g_seed = 0x2545F491u;

std::uint32_t Random()
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

std::int32_t Within(std::int32_t Low, std::int32_t High)
{
    return (std::int32_t)(Low + (std::int64_t)(Random() % (std::uint64_t)((std::int64_t)High - Low + 1)));
}

const BoxMatrix Plain = { BOX_SHAPE_UNIT, 0, 0, BOX_SHAPE_UNIT, 0, 0 };

BoxShape &ShapeRoom::Begin(const BoxMatrix &M)
{
    BoxShapeBegin(&Shape, Edges.data(), (std::uint32_t)Edges.size(), &M);
    return Shape;
}

Coverage CoverageOf(BoxShape &S, std::int32_t Width, std::int32_t Height)
{
    Coverage C;
    C.Width  = Width;
    C.Height = Height;
    C.Shares.assign((std::size_t)Width * Height, 0);
    BoxShapeClose(&S);
    const BoxRect Held = BoxShapeReach(&S, BoxRect{ 0, 0, Width, Height });
    if (!BoxShapeDrawable(&S) || Held.Width <= 0 || Held.Height <= 0) return C;
    std::vector<std::int32_t> Cells((std::size_t)Held.Width + 2);
    BoxShapeSweep             Sweep;
    BoxShapeSweepBegin(&Sweep, &S, Cells.data(), Held.X, Held.X + Held.Width);
    for (std::int32_t Y = Held.Y; Y < Held.Y + Held.Height; Y++) {
        BoxShapeSweepRow(&Sweep, Y);
        std::int64_t Run = Cells[0];
        for (std::int32_t X = Held.X; X < Held.X + Held.Width; X++) {
            Run += Cells[(std::size_t)(1 + X - Held.X)];
            std::int64_t Covered = Run < 0 ? -Run : Run;
            if (S.Rule == BOX_SHAPE_EVENODD) {
                Covered %= 2 * 65536;
                if (Covered > 65536) Covered = 2 * 65536 - Covered;
            }
            C.Area += (double)(Covered > 65536 ? 65536 : Covered) / 65536.0;
            C.Shares[(std::size_t)Y * Width + X] = (std::uint8_t)BoxShapeCoverage(Run, S.Rule);
        }
    }
    return C;
}

bool Near(double Got, double Want, double Share)
{
    return std::fabs(Got - Want) <= std::fabs(Want) * Share;
}

}  // namespace stand

int main(int Argc, char **Argv)
{
    if (Argc == 3 && std::strcmp(Argv[1], "sheet") == 0) return stand::WriteSheet(Argv[2]) ? 0 : 1;
    if ((Argc == 3 || Argc == 4) && std::strcmp(Argv[1], "text-sheet") == 0)
        return stand::WriteTextSheet(Argv[2], Argc == 4 ? Argv[3] : nullptr) ? 0 : 1;
    if (Argc == 2 && std::strcmp(Argv[1], "speed") == 0) {
        stand::SaySpeed();
        stand::SayTextSpeed();
        return 0;
    }
    if (Argc != 2 || std::strcmp(Argv[1], "checks") != 0) {
        std::fprintf(stderr, "usage: draw_stand checks | speed | sheet OUT.ppm\n");
        return 2;
    }
    stand::Tally Count;
    stand::CheckShapes(Count);
    stand::CheckStrokes(Count);
    stand::CheckPaints(Count);
    stand::CheckEngine(Count);
    stand::CheckLanes(Count);
    stand::CheckShadows(Count);
    stand::CheckText(Count);
    std::printf("draw stand: %d held, %d failed\n", Count.Held, Count.Failed);
    return Count.Failed ? 1 : 0;
}
