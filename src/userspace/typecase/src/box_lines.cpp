/* box_lines.cpp — the lines of frames, U+2500..U+257F, worked out for the
 * cell (box_lines.h). */

#include "box_lines.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace typecase {

namespace {

enum Weight : std::uint8_t { None, Light, Heavy, Double };

enum class LineKind : std::uint8_t {
    Plain,     /* arms from the edges to the middle */
    Dash2,     /* a straight line broken into two, three or four dashes */
    Dash3,
    Dash4,
    Arc,       /* two arms joined by a quarter circle */
    Rising,    /* the diagonals, corner to corner */
    Falling,
    Cross,
};

/* Each character's arms — the line it draws from the middle to each edge —
 * and its kind; read off the characters' names. */
struct Lines {
    std::uint8_t Left, Up, Right, Down;
    LineKind     Kind;
};

constexpr std::array<Lines, 128> Table = {{
    {1, 0, 1, 0, LineKind::Plain},   /* U+2500 ─ light horizontal */
    {2, 0, 2, 0, LineKind::Plain},   /* U+2501 ━ heavy horizontal */
    {0, 1, 0, 1, LineKind::Plain},   /* U+2502 │ light vertical */
    {0, 2, 0, 2, LineKind::Plain},   /* U+2503 ┃ heavy vertical */
    {1, 0, 1, 0, LineKind::Dash3},   /* U+2504 ┄ light triple dash horizontal */
    {2, 0, 2, 0, LineKind::Dash3},   /* U+2505 ┅ heavy triple dash horizontal */
    {0, 1, 0, 1, LineKind::Dash3},   /* U+2506 ┆ light triple dash vertical */
    {0, 2, 0, 2, LineKind::Dash3},   /* U+2507 ┇ heavy triple dash vertical */
    {1, 0, 1, 0, LineKind::Dash4},   /* U+2508 ┈ light quadruple dash horizontal */
    {2, 0, 2, 0, LineKind::Dash4},   /* U+2509 ┉ heavy quadruple dash horizontal */
    {0, 1, 0, 1, LineKind::Dash4},   /* U+250A ┊ light quadruple dash vertical */
    {0, 2, 0, 2, LineKind::Dash4},   /* U+250B ┋ heavy quadruple dash vertical */
    {0, 0, 1, 1, LineKind::Plain},   /* U+250C ┌ light down and right */
    {0, 0, 2, 1, LineKind::Plain},   /* U+250D ┍ down light and right heavy */
    {0, 0, 1, 2, LineKind::Plain},   /* U+250E ┎ down heavy and right light */
    {0, 0, 2, 2, LineKind::Plain},   /* U+250F ┏ heavy down and right */
    {1, 0, 0, 1, LineKind::Plain},   /* U+2510 ┐ light down and left */
    {2, 0, 0, 1, LineKind::Plain},   /* U+2511 ┑ down light and left heavy */
    {1, 0, 0, 2, LineKind::Plain},   /* U+2512 ┒ down heavy and left light */
    {2, 0, 0, 2, LineKind::Plain},   /* U+2513 ┓ heavy down and left */
    {0, 1, 1, 0, LineKind::Plain},   /* U+2514 └ light up and right */
    {0, 1, 2, 0, LineKind::Plain},   /* U+2515 ┕ up light and right heavy */
    {0, 2, 1, 0, LineKind::Plain},   /* U+2516 ┖ up heavy and right light */
    {0, 2, 2, 0, LineKind::Plain},   /* U+2517 ┗ heavy up and right */
    {1, 1, 0, 0, LineKind::Plain},   /* U+2518 ┘ light up and left */
    {2, 1, 0, 0, LineKind::Plain},   /* U+2519 ┙ up light and left heavy */
    {1, 2, 0, 0, LineKind::Plain},   /* U+251A ┚ up heavy and left light */
    {2, 2, 0, 0, LineKind::Plain},   /* U+251B ┛ heavy up and left */
    {0, 1, 1, 1, LineKind::Plain},   /* U+251C ├ light vertical and right */
    {0, 1, 2, 1, LineKind::Plain},   /* U+251D ┝ vertical light and right heavy */
    {0, 2, 1, 1, LineKind::Plain},   /* U+251E ┞ up heavy and right down light */
    {0, 1, 1, 2, LineKind::Plain},   /* U+251F ┟ down heavy and right up light */
    {0, 2, 1, 2, LineKind::Plain},   /* U+2520 ┠ vertical heavy and right light */
    {0, 2, 2, 1, LineKind::Plain},   /* U+2521 ┡ down light and right up heavy */
    {0, 1, 2, 2, LineKind::Plain},   /* U+2522 ┢ up light and right down heavy */
    {0, 2, 2, 2, LineKind::Plain},   /* U+2523 ┣ heavy vertical and right */
    {1, 1, 0, 1, LineKind::Plain},   /* U+2524 ┤ light vertical and left */
    {2, 1, 0, 1, LineKind::Plain},   /* U+2525 ┥ vertical light and left heavy */
    {1, 2, 0, 1, LineKind::Plain},   /* U+2526 ┦ up heavy and left down light */
    {1, 1, 0, 2, LineKind::Plain},   /* U+2527 ┧ down heavy and left up light */
    {1, 2, 0, 2, LineKind::Plain},   /* U+2528 ┨ vertical heavy and left light */
    {2, 2, 0, 1, LineKind::Plain},   /* U+2529 ┩ down light and left up heavy */
    {2, 1, 0, 2, LineKind::Plain},   /* U+252A ┪ up light and left down heavy */
    {2, 2, 0, 2, LineKind::Plain},   /* U+252B ┫ heavy vertical and left */
    {1, 0, 1, 1, LineKind::Plain},   /* U+252C ┬ light down and horizontal */
    {2, 0, 1, 1, LineKind::Plain},   /* U+252D ┭ left heavy and right down light */
    {1, 0, 2, 1, LineKind::Plain},   /* U+252E ┮ right heavy and left down light */
    {2, 0, 2, 1, LineKind::Plain},   /* U+252F ┯ down light and horizontal heavy */
    {1, 0, 1, 2, LineKind::Plain},   /* U+2530 ┰ down heavy and horizontal light */
    {2, 0, 1, 2, LineKind::Plain},   /* U+2531 ┱ right light and left down heavy */
    {1, 0, 2, 2, LineKind::Plain},   /* U+2532 ┲ left light and right down heavy */
    {2, 0, 2, 2, LineKind::Plain},   /* U+2533 ┳ heavy down and horizontal */
    {1, 1, 1, 0, LineKind::Plain},   /* U+2534 ┴ light up and horizontal */
    {2, 1, 1, 0, LineKind::Plain},   /* U+2535 ┵ left heavy and right up light */
    {1, 1, 2, 0, LineKind::Plain},   /* U+2536 ┶ right heavy and left up light */
    {2, 1, 2, 0, LineKind::Plain},   /* U+2537 ┷ up light and horizontal heavy */
    {1, 2, 1, 0, LineKind::Plain},   /* U+2538 ┸ up heavy and horizontal light */
    {2, 2, 1, 0, LineKind::Plain},   /* U+2539 ┹ right light and left up heavy */
    {1, 2, 2, 0, LineKind::Plain},   /* U+253A ┺ left light and right up heavy */
    {2, 2, 2, 0, LineKind::Plain},   /* U+253B ┻ heavy up and horizontal */
    {1, 1, 1, 1, LineKind::Plain},   /* U+253C ┼ light vertical and horizontal */
    {2, 1, 1, 1, LineKind::Plain},   /* U+253D ┽ left heavy and right vertical light */
    {1, 1, 2, 1, LineKind::Plain},   /* U+253E ┾ right heavy and left vertical light */
    {2, 1, 2, 1, LineKind::Plain},   /* U+253F ┿ vertical light and horizontal heavy */
    {1, 2, 1, 1, LineKind::Plain},   /* U+2540 ╀ up heavy and down horizontal light */
    {1, 1, 1, 2, LineKind::Plain},   /* U+2541 ╁ down heavy and up horizontal light */
    {1, 2, 1, 2, LineKind::Plain},   /* U+2542 ╂ vertical heavy and horizontal light */
    {2, 2, 1, 1, LineKind::Plain},   /* U+2543 ╃ left up heavy and right down light */
    {1, 2, 2, 1, LineKind::Plain},   /* U+2544 ╄ right up heavy and left down light */
    {2, 1, 1, 2, LineKind::Plain},   /* U+2545 ╅ left down heavy and right up light */
    {1, 1, 2, 2, LineKind::Plain},   /* U+2546 ╆ right down heavy and left up light */
    {2, 2, 2, 1, LineKind::Plain},   /* U+2547 ╇ down light and up horizontal heavy */
    {2, 1, 2, 2, LineKind::Plain},   /* U+2548 ╈ up light and down horizontal heavy */
    {2, 2, 1, 2, LineKind::Plain},   /* U+2549 ╉ right light and left vertical heavy */
    {1, 2, 2, 2, LineKind::Plain},   /* U+254A ╊ left light and right vertical heavy */
    {2, 2, 2, 2, LineKind::Plain},   /* U+254B ╋ heavy vertical and horizontal */
    {1, 0, 1, 0, LineKind::Dash2},   /* U+254C ╌ light double dash horizontal */
    {2, 0, 2, 0, LineKind::Dash2},   /* U+254D ╍ heavy double dash horizontal */
    {0, 1, 0, 1, LineKind::Dash2},   /* U+254E ╎ light double dash vertical */
    {0, 2, 0, 2, LineKind::Dash2},   /* U+254F ╏ heavy double dash vertical */
    {3, 0, 3, 0, LineKind::Plain},   /* U+2550 ═ double horizontal */
    {0, 3, 0, 3, LineKind::Plain},   /* U+2551 ║ double vertical */
    {0, 0, 3, 1, LineKind::Plain},   /* U+2552 ╒ down single and right double */
    {0, 0, 1, 3, LineKind::Plain},   /* U+2553 ╓ down double and right single */
    {0, 0, 3, 3, LineKind::Plain},   /* U+2554 ╔ double down and right */
    {3, 0, 0, 1, LineKind::Plain},   /* U+2555 ╕ down single and left double */
    {1, 0, 0, 3, LineKind::Plain},   /* U+2556 ╖ down double and left single */
    {3, 0, 0, 3, LineKind::Plain},   /* U+2557 ╗ double down and left */
    {0, 1, 3, 0, LineKind::Plain},   /* U+2558 ╘ up single and right double */
    {0, 3, 1, 0, LineKind::Plain},   /* U+2559 ╙ up double and right single */
    {0, 3, 3, 0, LineKind::Plain},   /* U+255A ╚ double up and right */
    {3, 1, 0, 0, LineKind::Plain},   /* U+255B ╛ up single and left double */
    {1, 3, 0, 0, LineKind::Plain},   /* U+255C ╜ up double and left single */
    {3, 3, 0, 0, LineKind::Plain},   /* U+255D ╝ double up and left */
    {0, 1, 3, 1, LineKind::Plain},   /* U+255E ╞ vertical single and right double */
    {0, 3, 1, 3, LineKind::Plain},   /* U+255F ╟ vertical double and right single */
    {0, 3, 3, 3, LineKind::Plain},   /* U+2560 ╠ double vertical and right */
    {3, 1, 0, 1, LineKind::Plain},   /* U+2561 ╡ vertical single and left double */
    {1, 3, 0, 3, LineKind::Plain},   /* U+2562 ╢ vertical double and left single */
    {3, 3, 0, 3, LineKind::Plain},   /* U+2563 ╣ double vertical and left */
    {3, 0, 3, 1, LineKind::Plain},   /* U+2564 ╤ down single and horizontal double */
    {1, 0, 1, 3, LineKind::Plain},   /* U+2565 ╥ down double and horizontal single */
    {3, 0, 3, 3, LineKind::Plain},   /* U+2566 ╦ double down and horizontal */
    {3, 1, 3, 0, LineKind::Plain},   /* U+2567 ╧ up single and horizontal double */
    {1, 3, 1, 0, LineKind::Plain},   /* U+2568 ╨ up double and horizontal single */
    {3, 3, 3, 0, LineKind::Plain},   /* U+2569 ╩ double up and horizontal */
    {3, 1, 3, 1, LineKind::Plain},   /* U+256A ╪ vertical single and horizontal double */
    {1, 3, 1, 3, LineKind::Plain},   /* U+256B ╫ vertical double and horizontal single */
    {3, 3, 3, 3, LineKind::Plain},   /* U+256C ╬ double vertical and horizontal */
    {0, 0, 1, 1, LineKind::Arc},   /* U+256D ╭ light arc down and right */
    {1, 0, 0, 1, LineKind::Arc},   /* U+256E ╮ light arc down and left */
    {1, 1, 0, 0, LineKind::Arc},   /* U+256F ╯ light arc up and left */
    {0, 1, 1, 0, LineKind::Arc},   /* U+2570 ╰ light arc up and right */
    {0, 0, 0, 0, LineKind::Rising},   /* U+2571 ╱ light diagonal upper right to lower left */
    {0, 0, 0, 0, LineKind::Falling},   /* U+2572 ╲ light diagonal upper left to lower right */
    {0, 0, 0, 0, LineKind::Cross},   /* U+2573 ╳ light diagonal cross */
    {1, 0, 0, 0, LineKind::Plain},   /* U+2574 ╴ light left */
    {0, 1, 0, 0, LineKind::Plain},   /* U+2575 ╵ light up */
    {0, 0, 1, 0, LineKind::Plain},   /* U+2576 ╶ light right */
    {0, 0, 0, 1, LineKind::Plain},   /* U+2577 ╷ light down */
    {2, 0, 0, 0, LineKind::Plain},   /* U+2578 ╸ heavy left */
    {0, 2, 0, 0, LineKind::Plain},   /* U+2579 ╹ heavy up */
    {0, 0, 2, 0, LineKind::Plain},   /* U+257A ╺ heavy right */
    {0, 0, 0, 2, LineKind::Plain},   /* U+257B ╻ heavy down */
    {1, 0, 2, 0, LineKind::Plain},   /* U+257C ╼ light left and heavy right */
    {0, 1, 0, 2, LineKind::Plain},   /* U+257D ╽ light up and heavy down */
    {2, 0, 1, 0, LineKind::Plain},   /* U+257E ╾ heavy left and light right */
    {0, 2, 0, 1, LineKind::Plain},   /* U+257F ╿ heavy up and light down */
}};

/* Where lines lie across one axis of the cell, Length pixels long: a line
 * of a weight starts where it stands in the middle, and a double line's
 * two strokes stand a light line either side of a light middle. */
struct Axis {
    std::int32_t Length, Light, Low, High;

    std::int32_t Thick(std::uint8_t Of) const noexcept { return Of == Heavy ? 2 * Light : Light; }
    std::int32_t Start(std::uint8_t Of) const noexcept { return (Length - Thick(Of)) / 2; }
};

Axis AxisOf(std::int32_t Length, std::int32_t Light)
{
    const std::int32_t Low = (Length - 3 * Light) / 2;
    return Axis{Length, Light, Low, Low + 2 * Light};
}

bool Single(std::uint8_t Of) noexcept { return Of == Light || Of == Heavy; }

/* How far toward the middle an arm reaches, along its own axis (Along), the
 * arms across it being Near and Far — for a stroke of a double arm, the arm
 * on its side and the one opposite; for any other arm, either way round.
 * Toward is true for an arm that runs from the far edge (right, down): its
 * reach is where it begins; else where it ends. */
std::int32_t Reach(const Axis &Along, std::uint8_t Self, std::uint8_t Opposite, std::uint8_t Near, std::uint8_t Far,
                   bool Toward)
{
    const auto Begin = [&](std::int32_t From, std::int32_t Thick) { return Toward ? From : From + Thick; };
    if (Self == Double) {
        if (Near == Double) return Toward ? Along.High : Along.Low + Along.Light;     /* the inner corner */
        if (Far == Double) return Toward ? Along.Low : Along.High + Along.Light;      /* the outer corner */
        std::uint8_t Stem = Light;   /* the stem the strokes touch: light when there is none */
        for (const std::uint8_t Across : {Near, Far})
            if (Single(Across)) Stem = std::max(Stem, Across);
        return Begin(Along.Start(Stem), Along.Thick(Stem));
    }
    if (Near == Double || Far == Double) {
        if (Single(Opposite)) return Begin(Along.Start(Light), Along.Light);          /* straight through */
        if (Near == Double && Far == Double) return Toward ? Along.High : Along.Low + Along.Light;
        return Toward ? Along.Low : Along.High + Along.Light;                          /* to the far stroke */
    }
    if (Near == None && Far == None) return Begin(Along.Start(Self), Along.Thick(Self));
    std::int32_t Low = Along.Length, High = 0;
    for (const std::uint8_t Stem : {Near, Far}) {
        if (Stem == None) continue;
        Low  = std::min(Low, Along.Start(Stem));
        High = std::max(High, Along.Start(Stem) + Along.Thick(Stem));
    }
    return Toward ? Low : High;
}

/* The arms of a plain character: each a stroke, or two for a double arm,
 * from its edge to where it meets the arms across it. */
void Arms(const Lines &Of, const Axis &X, const Axis &Y, CellInk &Ink)
{
    const auto Horizontal = [&](std::uint8_t Self, std::uint8_t Opposite, bool FromRight) {
        if (Self == None) return;
        const auto Stroke = [&](std::int32_t Row, std::int32_t Thick, std::uint8_t Near, std::uint8_t Far) {
            const std::int32_t To = Reach(X, Self, Opposite, Near, Far, FromRight);
            if (FromRight) Ink.Fill(To, Row, X.Length, Row + Thick);
            else Ink.Fill(0, Row, To, Row + Thick);
        };
        if (Self == Double) {
            Stroke(Y.Low, Y.Light, Of.Up, Of.Down);
            Stroke(Y.High, Y.Light, Of.Down, Of.Up);
        } else {
            Stroke(Y.Start(Self), Y.Thick(Self), Of.Up, Of.Down);
        }
    };
    const auto Vertical = [&](std::uint8_t Self, std::uint8_t Opposite, bool FromBottom) {
        if (Self == None) return;
        const auto Stroke = [&](std::int32_t Column, std::int32_t Thick, std::uint8_t Near, std::uint8_t Far) {
            const std::int32_t To = Reach(Y, Self, Opposite, Near, Far, FromBottom);
            if (FromBottom) Ink.Fill(Column, To, Column + Thick, Y.Length);
            else Ink.Fill(Column, 0, Column + Thick, To);
        };
        if (Self == Double) {
            Stroke(X.Low, X.Light, Of.Left, Of.Right);
            Stroke(X.High, X.Light, Of.Right, Of.Left);
        } else {
            Stroke(X.Start(Self), X.Thick(Self), Of.Left, Of.Right);
        }
    };
    Horizontal(Of.Left, Of.Right, false);
    Horizontal(Of.Right, Of.Left, true);
    Vertical(Of.Up, Of.Down, false);
    Vertical(Of.Down, Of.Up, true);
}

/* A straight line broken into Count dashes, one to each Count-th of the
 * cell, a third of each left as the gap — so dashes keep their rhythm from
 * one cell into the next. */
void Dashes(const Lines &Of, std::int32_t Count, const Axis &X, const Axis &Y, CellInk &Ink)
{
    const bool         Across = Of.Left != None;
    const std::uint8_t Self   = Across ? Of.Left : Of.Up;
    const Axis        &Along  = Across ? X : Y;
    const Axis        &Over   = Across ? Y : X;
    for (std::int32_t Index = 0; Index < Count; Index++) {
        const std::int32_t From = Edge(Index, Count, Along.Length), To = Edge(Index + 1, Count, Along.Length);
        const std::int32_t Gap  = std::max(1, (To - From) / 3);
        const std::int32_t Low = From + Gap / 2, High = To - (Gap - Gap / 2);
        const std::int32_t Start = Over.Start(Self), End = Start + Over.Thick(Self);
        if (Across) Ink.Fill(Low, Start, High, End);
        else Ink.Fill(Start, Low, End, High);
    }
}

/* A quarter of a ring, centred at (X, Y), between radii Inner and Outer,
 * from angle From through a quarter turn (radians, y down): four curves to
 * each side, which keep within a hair of the circle. */
Outline QuarterRing(float X, float Y, float Inner, float Outer, float From)
{
    constexpr int   Pieces = 4;
    constexpr float Turn   = 1.57079632679f / Pieces;
    Outline         Shape;
    const auto      Side = [&](float Radius, int First, int Step) {
        for (int Index = 0; Index < Pieces; Index++) {
            const float A = From + Turn * static_cast<float>(First + Step * Index);
            const float B = A + Turn * static_cast<float>(Step);
            const float Reach = Radius / std::cos(Turn / 2);
            Shape.Steps.push_back(Step::Quad);
            Shape.Points.push_back(Point{X + Reach * std::cos((A + B) / 2), Y + Reach * std::sin((A + B) / 2)});
            Shape.Points.push_back(Point{X + Radius * std::cos(B), Y + Radius * std::sin(B)});
        }
    };
    Shape.Steps.push_back(Step::Move);
    Shape.Points.push_back(Point{X + Outer * std::cos(From), Y + Outer * std::sin(From)});
    Side(Outer, 0, 1);
    Shape.Steps.push_back(Step::Line);
    Shape.Points.push_back(Point{X + Inner * std::cos(From + Turn * Pieces), Y + Inner * std::sin(From + Turn * Pieces)});
    Side(Inner, Pieces, -1);
    return Shape;
}

/* A rounded corner: the two arms' strokes meet in a quarter circle whose
 * ends lie on them, the rest of each arm straight to its edge. */
void Arc(const Lines &Of, const Axis &X, const Axis &Y, CellInk &Ink, Rasterizer &Pen)
{
    const float Thin = static_cast<float>(X.Light);
    const float Mx = static_cast<float>(X.Start(Light)) + Thin / 2, My = static_cast<float>(Y.Start(Light)) + Thin / 2;
    const bool  Right = Of.Right != None, Down = Of.Down != None;
    const float Radius = std::min(Right ? static_cast<float>(X.Length) - Mx : Mx, Down ? static_cast<float>(Y.Length) - My : My);
    const float Cx = Right ? Mx + Radius : Mx - Radius, Cy = Down ? My + Radius : My - Radius;
    /* The quarter faces the corner opposite the arms. */
    const float From = Right ? (Down ? 3.14159265f : 1.57079632f) : (Down ? 4.71238898f : 0.0f);
    Ink.Smooth(QuarterRing(Cx, Cy, Radius - Thin / 2, Radius + Thin / 2, From), Pen);

    /* The straight rest of each arm begins in the pixel the ring ends in,
     * so no sliver of it is left half inked. */
    const std::int32_t Column = X.Start(Light), Row = Y.Start(Light);
    const auto Low  = [](float Value) { return static_cast<std::int32_t>(std::floor(Value)); };
    const auto High = [](float Value) { return static_cast<std::int32_t>(std::ceil(Value)); };
    if (Down) Ink.Fill(Column, Low(Cy), Column + X.Light, Y.Length);
    else Ink.Fill(Column, 0, Column + X.Light, High(Cy));
    if (Right) Ink.Fill(Low(Cx), Row, X.Length, Row + Y.Light);
    else Ink.Fill(0, Row, High(Cx), Row + Y.Light);
}

/* A diagonal from corner to corner, a light line thick across its slant. */
void Diagonal(bool Rising, const Axis &X, const Axis &Y, CellInk &Ink, Rasterizer &Pen)
{
    const float W = static_cast<float>(X.Length), H = static_cast<float>(Y.Length);
    const float Half = static_cast<float>(X.Light) / 2 * std::hypot(W, H) / H;
    Outline     Band;
    Band.Steps = {Step::Move, Step::Line, Step::Line, Step::Line};
    if (Rising) Band.Points = {Point{W - Half, 0}, Point{W + Half, 0}, Point{Half, H}, Point{-Half, H}};
    else Band.Points = {Point{-Half, 0}, Point{Half, 0}, Point{W + Half, H}, Point{W - Half, H}};
    Ink.Smooth(Band, Pen);
}

}  // namespace

bool IsBoxLine(char32_t Character) noexcept { return Character >= 0x2500 && Character <= 0x257F; }

void DrawBoxLine(char32_t Character, CellInk &Ink, Rasterizer &Pen)
{
    const Lines &Of    = Table[Character - 0x2500];
    const auto   Light = std::max(1, (Ink.Width + 5) / 10);
    const Axis   X = AxisOf(Ink.Width, Light), Y = AxisOf(Ink.Height, Light);
    switch (Of.Kind) {
    case LineKind::Plain:   Arms(Of, X, Y, Ink); break;
    case LineKind::Dash2:   Dashes(Of, 2, X, Y, Ink); break;
    case LineKind::Dash3:   Dashes(Of, 3, X, Y, Ink); break;
    case LineKind::Dash4:   Dashes(Of, 4, X, Y, Ink); break;
    case LineKind::Arc:     Arc(Of, X, Y, Ink, Pen); break;
    case LineKind::Rising:  Diagonal(true, X, Y, Ink, Pen); break;
    case LineKind::Falling: Diagonal(false, X, Y, Ink, Pen); break;
    case LineKind::Cross:
        Diagonal(true, X, Y, Ink, Pen);
        Diagonal(false, X, Y, Ink, Pen);
        break;
    }
}

}  // namespace typecase
