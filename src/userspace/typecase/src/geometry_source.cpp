/* geometry_source.cpp — characters worked out for the cell. */

#include "typecase/geometry_source.h"

#include <algorithm>
#include <cstdint>

#include "box_lines.h"
#include "boxos_mosaic.h"
#include "cell_ink.h"
#include "wedges.h"

namespace typecase {

namespace {

constexpr char32_t LightShade = 0x2591, DenseShade = 0x2593;
constexpr char32_t BrailleFirst = 0x2800, BrailleLast = 0x28FF;

/* The blocks that are no mosaic: a part of the cell in eighths, from one of
 * its sides — the rows from the bottom, the columns from the left, or the
 * eighth at the top or the right. */
struct Eighths {
    char32_t     Character;
    char         Side;     /* 'B'ottom, 'L'eft, 'T'op, 'R'ight */
    std::int32_t Parts;
};

constexpr Eighths Blocks[] = {
    {0x2581, 'B', 1}, {0x2583, 'B', 3}, {0x2585, 'B', 5}, {0x2587, 'B', 7},
    {0x2589, 'L', 7}, {0x258A, 'L', 6}, {0x258B, 'L', 5}, {0x258D, 'L', 3},
    {0x258E, 'L', 2}, {0x258F, 'L', 1}, {0x2594, 'T', 1}, {0x2595, 'R', 1},
};

const Eighths *EighthsOf(char32_t Character)
{
    for (const Eighths &Each : Blocks)
        if (Each.Character == Character) return &Each;
    return nullptr;
}

bool MosaicShape(char32_t Character, std::uint8_t &Rows, std::uint8_t &Pattern)
{
    return Character != U' ' && BoxMosaicShapeOf(static_cast<std::uint32_t>(Character), &Rows, &Pattern);
}

void DrawEighths(const Eighths &Of, CellInk &Ink)
{
    const std::int32_t W = Ink.Width, H = Ink.Height;
    switch (Of.Side) {
    case 'B': Ink.Fill(0, Edge(8 - Of.Parts, 8, H), W, H); break;
    case 'L': Ink.Fill(0, 0, Edge(Of.Parts, 8, W), H); break;
    case 'T': Ink.Fill(0, 0, W, Edge(Of.Parts, 8, H)); break;
    default:  Ink.Fill(Edge(8 - Of.Parts, 8, W), 0, W, H); break;
    }
}

/* A shade: every pixel of a 2x2 tile's quarter, half or three quarters —
 * one in four, a checker, all but one in four. */
void DrawShade(char32_t Character, CellInk &Ink)
{
    const int Level = static_cast<int>(Character - LightShade);
    for (std::int32_t Y = 0; Y < Ink.Height; Y++)
        for (std::int32_t X = 0; X < Ink.Width; X++) {
            const bool Odd = X % 2 != 0, Lower = Y % 2 != 0;
            const bool Inked = Level == 0 ? !Odd && !Lower : Level == 1 ? Odd == Lower : Odd == Lower || Odd;
            if (Inked) Ink.At(X, Y) = 255;
        }
}

void DrawMosaic(std::uint8_t Rows, std::uint8_t Pattern, CellInk &Ink)
{
    for (std::int32_t Index = 0; Index < 2 * Rows; Index++) {
        if (!(Pattern & (1u << Index))) continue;
        const std::int32_t Row = Index / 2, Column = Index % 2;
        Ink.Fill(Edge(Column, 2, Ink.Width), Edge(Row, Rows, Ink.Height), Edge(Column + 1, 2, Ink.Width),
                 Edge(Row + 1, Rows, Ink.Height));
    }
}

/* Braille's dots, two across and four down, each in the middle of its part
 * of the cell, half as wide as the part: a square of pixels while that is
 * three or fewer, a round dot beyond. Dot 1 is the top left, 2 and 3 below
 * it, 4 to 6 the right column, 7 and 8 the bottom row. */
void DrawBraille(char32_t Character, CellInk &Ink, Rasterizer &Pen)
{
    constexpr std::int32_t Columns[8] = {0, 0, 0, 1, 1, 1, 0, 1}, Rows[8] = {0, 1, 2, 0, 1, 2, 3, 3};
    const std::int32_t     Dots = static_cast<std::int32_t>(Character - BrailleFirst);
    const std::int32_t     Side = std::max(1, std::min(Edge(1, 2, Ink.Width), Edge(1, 4, Ink.Height)) / 2);
    for (int Dot = 0; Dot < 8; Dot++) {
        if (!(Dots & (1 << Dot))) continue;
        const std::int32_t Left = Edge(Columns[Dot], 2, Ink.Width), Right = Edge(Columns[Dot] + 1, 2, Ink.Width);
        const std::int32_t Top = Edge(Rows[Dot], 4, Ink.Height), Bottom = Edge(Rows[Dot] + 1, 4, Ink.Height);
        if (Side <= 3) {
            const std::int32_t X = Left + (Right - Left - Side) / 2, Y = Top + (Bottom - Top - Side) / 2;
            Ink.Fill(X, Y, X + Side, Y + Side);
            continue;
        }
        const float Reach = static_cast<float>(Side) / 2;
        Ink.Smooth(Oval(static_cast<float>(Left + Right) / 2, static_cast<float>(Top + Bottom) / 2, Reach, Reach), Pen);
    }
}

}  // namespace

bool GeometrySource::Has(char32_t Character) const
{
    std::uint8_t Rows, Pattern;
    return IsBoxLine(Character) || IsWedge(Character) || (Character >= BrailleFirst && Character <= BrailleLast) ||
           (Character >= LightShade && Character <= DenseShade) || EighthsOf(Character) ||
           MosaicShape(Character, Rows, Pattern);
}

std::optional<GlyphImage> GeometrySource::Draw(char32_t Character, const Size &At)
{
    if (!Has(Character) || At.Width <= 0 || At.Height <= 0) return std::nullopt;
    CellInk      Ink(At.Width, At.Height);
    std::uint8_t Rows, Pattern;
    if (IsBoxLine(Character)) DrawBoxLine(Character, Ink, Pen);
    else if (IsWedge(Character)) DrawWedge(Character, Ink, Pen);
    else if (Character >= BrailleFirst && Character <= BrailleLast) DrawBraille(Character, Ink, Pen);
    else if (Character >= LightShade && Character <= DenseShade) DrawShade(Character, Ink);
    else if (const Eighths *Of = EighthsOf(Character)) DrawEighths(*Of, Ink);
    else if (MosaicShape(Character, Rows, Pattern)) DrawMosaic(Rows, Pattern, Ink);
    return std::move(Ink).Picture(At.Baseline);
}

}  // namespace typecase
