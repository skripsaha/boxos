/* shape.cpp — the pictures of the dice. */

#include "shape.h"

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <string>

#include "painter/tui/glyph.h"

#include "digits.h"

namespace dice {

namespace {

namespace tui = painter::tui;
using painter::core::Point;

struct Vertex {
    double U, V;
};

struct Segment {
    Vertex From, To;
};

constexpr std::array<Vertex, 3> TriangleCorners = {{{0.5, 0.04}, {0.97, 0.92}, {0.03, 0.92}}};
constexpr std::array<Vertex, 4> DiamondCorners  = {{{0.5, 0.0}, {1.0, 0.5}, {0.5, 1.0}, {0.0, 0.5}}};
constexpr std::array<Vertex, 4> KiteCorners     = {{{0.5, 0.0}, {0.98, 0.42}, {0.5, 1.0}, {0.02, 0.42}}};
constexpr std::array<Vertex, 5> PentagonCorners = {{{0.5, 0.0}, {0.98, 0.36}, {0.8, 0.95}, {0.2, 0.95}, {0.02, 0.36}}};
constexpr std::array<Vertex, 6> HexagonCorners  = {
    {{0.5, 0.0}, {0.95, 0.25}, {0.95, 0.75}, {0.5, 1.0}, {0.05, 0.75}, {0.05, 0.25}}};

// The triangle a d20 shows in the middle of its hexagon, and what joins it to the rim.
constexpr std::array<Segment, 6> IcosahedronLines = {{{{0.5, 0.22}, {0.84, 0.78}},
                                                      {{0.84, 0.78}, {0.16, 0.78}},
                                                      {{0.16, 0.78}, {0.5, 0.22}},
                                                      {{0.5, 0.22}, {0.5, 0.0}},
                                                      {{0.84, 0.78}, {0.95, 0.75}},
                                                      {{0.16, 0.78}, {0.05, 0.75}}}};

// The pips of a d6 on a grid of three by three, one bit each, row by row.
constexpr std::array<std::uint16_t, 7> PipGrids = {0, 0x010, 0x101, 0x111, 0x145, 0x155, 0x16D};

// Lines and rounded corners need room a small die has not.
constexpr int LeastSizeForLines  = 9;
constexpr int LeastSizeForRounds = 9;
// A number is drawn in pixels on a die this big, in a cell's glyph below it.
constexpr int LeastSizeForPixelNumbers = 13;

bool Convex(std::span<const Vertex> Corners, double U, double V)
{
    bool Left = false, Right = false;
    for (std::size_t Corner = 0; Corner < Corners.size(); Corner++) {
        const Vertex &From  = Corners[Corner];
        const Vertex &To    = Corners[(Corner + 1) % Corners.size()];
        const double  Cross = (To.U - From.U) * (V - From.V) - (To.V - From.V) * (U - From.U);
        if (Cross > 0) Left = true;
        if (Cross < 0) Right = true;
    }
    return !(Left && Right);
}

bool Inside(Shape Of, int Size, int X, int Y)
{
    if (X < 0 || Y < 0 || X >= Size || Y >= Size) return false;
    const double U = (X + 0.5) / Size;
    const double V = (Y + 0.5) / Size;
    switch (Of) {
    case Shape::Triangle: return Convex(TriangleCorners, U, V);
    case Shape::Diamond:  return Convex(DiamondCorners, U, V);
    case Shape::Kite:     return Convex(KiteCorners, U, V);
    case Shape::Pentagon: return Convex(PentagonCorners, U, V);
    case Shape::Hexagon:  return Convex(HexagonCorners, U, V);
    case Shape::Disc:     return (U - 0.5) * (U - 0.5) + (V - 0.5) * (V - 0.5) <= 0.25;
    case Shape::Cube: {
        const bool Corner = Size >= LeastSizeForRounds && (X == 0 || X == Size - 1) && (Y == 0 || Y == Size - 1);
        return !Corner;
    }
    }
    return false;
}

// Lighter toward the upper left, and the solids' two sides apart: the light
// falls on the upper half of a diamond, the left half of a kite or a triangle.
Color Shaded(Shape Of, int Size, int X, int Y, Color Body)
{
    const double U     = (X + 0.5) / Size;
    const double V     = (Y + 0.5) / Size;
    double       Light = (0.5 - (U + V) / 2) * 70;
    if (Of == Shape::Diamond) Light += V < 0.5 ? 25 : -25;
    if (Of == Shape::Kite || Of == Shape::Triangle) Light += U < 0.5 ? 25 : -25;
    if (Light >= 0) return Color::Mix(Body, painter::core::Colors::White, static_cast<std::uint8_t>(Light));
    return Color::Mix(Body, painter::core::Colors::Black, static_cast<std::uint8_t>(-Light));
}

bool OnEdge(Shape Of, int Size, int X, int Y)
{
    return !Inside(Of, Size, X - 1, Y) || !Inside(Of, Size, X + 1, Y) || !Inside(Of, Size, X, Y - 1) ||
           !Inside(Of, Size, X, Y + 1);
}

void Draw(const Canvas &On, Point At, int Size, Shape Of, Color Ink)
{
    for (int Y = 0; Y < Size; Y++)
        for (int X = 0; X < Size; X++)
            if (Inside(Of, Size, X, Y)) On.Plot(At.X + X, At.Y + Y, Ink);
}

void DrawLines(const Canvas &On, Point At, int Size, Color Ink)
{
    for (const Segment &Line : IcosahedronLines) {
        const int Steps = Size * 2;
        for (int Step = 0; Step <= Steps; Step++) {
            const double Along = static_cast<double>(Step) / Steps;
            const double U     = Line.From.U + (Line.To.U - Line.From.U) * Along;
            const double V     = Line.From.V + (Line.To.V - Line.From.V) * Along;
            On.Plot(At.X + static_cast<int>(U * (Size - 1) + 0.5), At.Y + static_cast<int>(V * (Size - 1) + 0.5), Ink);
        }
    }
}

void DrawPips(const Canvas &On, Point At, int Size, int Face, Color Ink)
{
    constexpr double Places[3] = {0.27, 0.5, 0.73};
    const int        Pip       = (Size + 2) / 6;
    const std::uint16_t Grid   = PipGrids[static_cast<std::size_t>(Face)];
    for (int Row = 0; Row < 3; Row++) {
        for (int Column = 0; Column < 3; Column++) {
            if (!(Grid & (1u << (Row * 3 + Column)))) continue;
            const int X = At.X + static_cast<int>(Places[Column] * Size + 0.5) - Pip / 2;
            const int Y = At.Y + static_cast<int>(Places[Row] * Size + 0.5) - Pip / 2;
            for (int Dy = 0; Dy < Pip; Dy++)
                for (int Dx = 0; Dx < Pip; Dx++) On.Plot(X + Dx, Y + Dy, Ink);
        }
    }
}

// The cells of a number, written over the picture: the glyph in the ink, the
// background what the two pixels under it were.
void DrawNumber(const Canvas &On, int CenterX, int CenterY, int Face, Color Ink)
{
    const std::string Text = std::format("{}", Face);
    const int         Row  = CenterY / 2;
    int               Column = CenterX - static_cast<int>(Text.size()) / 2;
    for (const char Digit : Text) {
        if (On.Allows(Column, Row * 2) && On.Allows(Column, Row * 2 + 1)) {
            tui::Cell Under = On.Out.Read({Column, Row});
            Under.Glyph      = Digit;
            Under.Background = Color::Mix(Under.Foreground, Under.Background, 128);
            Under.Foreground = Ink;
            On.Out.Put({Column, Row}, Under);
        }
        Column++;
    }
}

// The number in pixels; a plate of the body's color under it, when lines
// would run through it.
void DrawBigNumber(const Canvas &On, int CenterX, int CenterY, int Size, int Face, const DieStyle &Style, bool Plate)
{
    const std::string Text  = std::format("{}", Face);
    const int         Scale = std::max(1, std::min(Size / 9, Size * 6 / 10 / DigitsWide(Text.size(), 1)));
    const int         Wide  = DigitsWide(Text.size(), Scale);
    const int         High  = DigitsHigh(Scale);
    const int         Left  = CenterX - Wide / 2;
    const int         Top   = CenterY - High / 2;
    if (Plate) {
        for (int Y = Top - 1; Y <= Top + High; Y++)
            for (int X = Left - 1; X <= Left + Wide; X++) On.Plot(X, Y, Style.Body);
    }
    DrawDigits(On, {Left, Top}, Text, Scale, Style.Ink);
}

}  // namespace

Shape ShapeOf(int Sides)
{
    switch (Sides) {
    case 4:   return Shape::Triangle;
    case 6:   return Shape::Cube;
    case 8:   return Shape::Diamond;
    case 10:
    case 100: return Shape::Kite;
    case 12:  return Shape::Pentagon;
    case 20:  return Shape::Hexagon;
    default:  return Shape::Disc;
    }
}

int DigitsOf(int Number)
{
    return static_cast<int>(std::format("{}", Number).size());
}

void DrawShadow(const Canvas &On, Point At, int Size, Shape Of, Color Shade)
{
    Draw(On, {At.X + 1, At.Y + 1}, Size, Of, Shade);
}

void DrawDie(const Canvas &On, Point At, int Size, int Sides, int Face, const DieStyle &Style)
{
    const Shape Of = ShapeOf(Sides);
    for (int Y = 0; Y < Size; Y++) {
        for (int X = 0; X < Size; X++) {
            if (!Inside(Of, Size, X, Y)) continue;
            On.Plot(At.X + X, At.Y + Y, OnEdge(Of, Size, X, Y) ? Style.Edge : Shaded(Of, Size, X, Y, Style.Body));
        }
    }
    if (Of == Shape::Hexagon && Size >= LeastSizeForLines) DrawLines(On, At, Size, Style.Line);
    if (Of == Shape::Cube) {
        DrawPips(On, At, Size, Face, Style.Ink);
        return;
    }
    const int Down = Of == Shape::Triangle ? Size * 68 / 100 : Size / 2;
    if (Size >= LeastSizeForPixelNumbers)
        DrawBigNumber(On, At.X + Size / 2, At.Y + Down, Size, Face, Style, Of == Shape::Hexagon);
    else                                  DrawNumber(On, At.X + Size / 2, At.Y + Down, Face, Style.Ink);
}

void DrawTile(const Canvas &On, Point At, int Digits, int Face, const DieStyle &Style)
{
    const std::string Number = std::format("{}", Face);
    const std::size_t Width  = static_cast<std::size_t>(Digits) + 2;
    const std::size_t Left   = (Width - Number.size()) / 2;
    const std::string Text   = std::string(Left, ' ') + Number + std::string(Width - Left - Number.size(), ' ');
    const int         Row  = At.Y / 2;
    for (std::size_t Place = 0; Place < Text.size(); Place++) {
        const int Column = At.X + static_cast<int>(Place);
        if (!On.Allows(Column, Row * 2) || !On.Allows(Column, Row * 2 + 1)) continue;
        const bool Rim = Place == 0 || Place + 1 == Text.size();
        On.Out.Put({Column, Row}, tui::Cell{static_cast<char32_t>(Text[Place]), Style.Ink, Rim ? Style.Edge : Style.Body});
    }
}

}  // namespace dice
