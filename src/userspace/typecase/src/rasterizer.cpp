/* rasterizer.cpp — an outline to a picture of coverage. */

#include "typecase/rasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace typecase {

namespace {

/* How far a straight line may stray from the curve it follows, in pixels. */
constexpr float Tolerance = 1.0f / 16.0f;

/* No curve within a glyph's limit needs more lines than this; a control
 * point thrown far away by a damaged font is not followed further. */
constexpr int MostLines = 256;

Point Toward(Point From, Point To, float Share) noexcept
{
    return Point{From.X + (To.X - From.X) * Share, From.Y + (To.Y - From.Y) * Share};
}

}  // namespace

GlyphImage Rasterizer::Draw(const Outline &Shape, float Scale, float Rise, Bounds Limit, float Advance)
{
    GlyphImage Image;
    Image.Advance = Advance;

    /* Font units to pixels against the pen, y down; the ink's extent from
     * every point, controls too — a curve keeps within its controls. */
    std::vector<Point> At(Shape.Points.size());
    float Low = std::numeric_limits<float>::max(), High = std::numeric_limits<float>::lowest();
    float Up = Low, Down = High;
    for (std::size_t Index = 0; Index < At.size(); Index++) {
        At[Index] = Point{Shape.Points[Index].X * Scale, -Shape.Points[Index].Y * Rise};
        Low = std::min(Low, At[Index].X), High = std::max(High, At[Index].X);
        Up = std::min(Up, At[Index].Y), Down = std::max(Down, At[Index].Y);
    }
    if (At.empty()) return Image;

    const auto Clamp = [](float Value, std::int32_t Least, std::int32_t Most) {
        return static_cast<std::int32_t>(std::clamp(Value, static_cast<float>(Least), static_cast<float>(Most)));
    };
    const std::int32_t Left   = Clamp(std::floor(Low), Limit.Left, Limit.Right);
    const std::int32_t Right  = Clamp(std::ceil(High), Limit.Left, Limit.Right);
    const std::int32_t Top    = Clamp(std::floor(Up), Limit.Top, Limit.Bottom);
    const std::int32_t Bottom = Clamp(std::ceil(Down), Limit.Top, Limit.Bottom);
    if (Right <= Left || Bottom <= Top) return Image;

    Width  = Right - Left;
    Height = Bottom - Top;
    Area.assign(static_cast<std::size_t>(Width + 2) * static_cast<std::size_t>(Height), 0.0f);
    for (Point &Each : At) Each = Point{Each.X - static_cast<float>(Left), Each.Y - static_cast<float>(Top)};

    std::size_t Next = 0;
    Point       Start{}, Pen{};
    for (Step Each : Shape.Steps) {
        switch (Each) {
        case Step::Move:
            Edge(Pen, Start);
            Start = Pen = At[Next++];
            break;
        case Step::Line:
            Edge(Pen, At[Next]);
            Pen = At[Next++];
            break;
        case Step::Quad:
            Curve(Pen, At[Next], At[Next + 1]);
            Pen = At[Next + 1];
            Next += 2;
            break;
        }
    }
    Edge(Pen, Start);

    Image.Left   = Left;
    Image.Top    = Top;
    Image.Width  = Width;
    Image.Height = Height;
    Image.Coverage.resize(static_cast<std::size_t>(Width) * static_cast<std::size_t>(Height));
    for (std::int32_t Row = 0; Row < Height; Row++) {
        const float *Sums    = &Area[static_cast<std::size_t>(Row) * static_cast<std::size_t>(Width + 2)];
        float        Winding = 0;
        for (std::int32_t Column = 0; Column < Width; Column++) {
            Winding += Sums[Column];
            const float Share = std::min(std::fabs(Winding), 1.0f);
            Image.Coverage[static_cast<std::size_t>(Row) * static_cast<std::size_t>(Width) +
                           static_cast<std::size_t>(Column)] = static_cast<std::uint8_t>(Share * 255.0f + 0.5f);
        }
    }
    return Image;
}

void Rasterizer::Curve(Point From, Point Control, Point To)
{
    const float Bend = std::hypot(From.X - 2 * Control.X + To.X, From.Y - 2 * Control.Y + To.Y);
    /* A curve strays from its chord by a quarter of its bend; cut in N, by
     * a quarter of Bend / N^2. */
    const int Lines = static_cast<int>(
        std::clamp(std::ceil(std::sqrt(Bend / (4 * Tolerance))), 1.0f, static_cast<float>(MostLines)));
    Point Pen = From;
    for (int Index = 1; Index <= Lines; Index++) {
        const float Share = static_cast<float>(Index) / static_cast<float>(Lines);
        const Point Here  = Toward(Toward(From, Control, Share), Toward(Control, To, Share), Share);
        Edge(Pen, Here);
        Pen = Here;
    }
}

/* An edge, kept to the picture's columns: a part left of them winds every
 * pixel of its rows as an edge at the first column would, and a part right
 * of them winds none — each becomes an upright edge at that side, over the
 * same rows. Rows outside the picture Line passes over. */
void Rasterizer::Edge(Point From, Point To)
{
    const float Wide = static_cast<float>(Width);
    float       Cuts[4] = {0, 1, 1, 1};
    int         Count   = 1;
    for (float Side : {0.0f, Wide}) {
        if ((From.X < Side) != (To.X < Side)) {
            const float Share = (Side - From.X) / (To.X - From.X);
            if (Share > 0 && Share < 1) Cuts[Count++] = Share;
        }
    }
    Cuts[Count++] = 1;
    std::sort(Cuts, Cuts + Count);

    for (int Index = 0; Index + 1 < Count; Index++) {
        Point       A   = Toward(From, To, Cuts[Index]);
        Point       B   = Toward(From, To, Cuts[Index + 1]);
        const float Mid = (A.X + B.X) / 2;
        if (Mid < 0) {
            A.X = B.X = 0;
        } else if (Mid > Wide) {
            A.X = B.X = Wide;
        } else {
            A.X = std::clamp(A.X, 0.0f, Wide);
            B.X = std::clamp(B.X, 0.0f, Wide);
        }
        Line(A, B);
    }
}

/* An edge within the picture's columns, its area left in each pixel of its
 * rows: the share of the pixel's height it spans, split between the pixel
 * it crosses and the one past it by where it crosses — a summed row then
 * holds the winding left of every pixel and the part of it each pixel has. */
void Rasterizer::Line(Point From, Point To)
{
    if (From.Y == To.Y) return;
    float Direction = 1;
    if (From.Y > To.Y) {
        std::swap(From, To);
        Direction = -1;
    }
    /* Both ends lie within the columns, and so does every x between them;
     * the clamps keep a rounding from stepping a hair outside. */
    const float Wide  = static_cast<float>(Width), Tall = static_cast<float>(Height);
    const float Slope = (To.X - From.X) / (To.Y - From.Y);
    const int   First = static_cast<int>(std::floor(std::clamp(From.Y, 0.0f, Tall)));
    const int   Last  = static_cast<int>(std::ceil(std::clamp(To.Y, 0.0f, Tall)));
    float       X     = std::clamp(
        From.X + (static_cast<float>(First) > From.Y ? (static_cast<float>(First) - From.Y) * Slope : 0), 0.0f, Wide);

    for (int Row = First; Row < Last; Row++) {
        float *Sums = &Area[static_cast<std::size_t>(Row) * static_cast<std::size_t>(Width + 2)];
        const float Span =
            std::min(static_cast<float>(Row + 1), To.Y) - std::max(static_cast<float>(Row), From.Y);
        const float Then  = std::clamp(X + Slope * Span, 0.0f, Wide);
        const float Share = Span * Direction;
        const float Near  = std::min(X, Then), Far = std::max(X, Then);
        const float NearFloor = std::floor(Near), FarCeil = std::ceil(Far);
        const int   NearCol = static_cast<int>(NearFloor), FarCol = static_cast<int>(FarCeil);

        if (FarCol <= NearCol + 1) {
            const float Into = (X + Then) / 2 - NearFloor;
            Sums[NearCol] += Share - Share * Into;
            Sums[NearCol + 1] += Share * Into;
        } else {
            /* Across several columns: the edge's area in each grows by Step
             * a column, a triangle at either end. */
            const float Step     = 1 / (Far - Near);
            const float NearPart = Near - NearFloor;
            const float NearArea = Step * (1 - NearPart) * (1 - NearPart) / 2;
            const float FarPart  = Far - FarCeil + 1;
            const float FarArea  = Step * FarPart * FarPart / 2;
            Sums[NearCol] += Share * NearArea;
            if (FarCol == NearCol + 2) {
                Sums[NearCol + 1] += Share * (1 - NearArea - FarArea);
            } else {
                const float Second = Step * (1.5f - NearPart);
                Sums[NearCol + 1] += Share * (Second - NearArea);
                for (int Column = NearCol + 2; Column < FarCol - 1; Column++) Sums[Column] += Share * Step;
                const float BeforeLast = Second + static_cast<float>(FarCol - NearCol - 3) * Step;
                Sums[FarCol - 1] += Share * (1 - BeforeLast - FarArea);
            }
            Sums[FarCol] += Share * FarArea;
        }
        X = Then;
    }
}

}  // namespace typecase
