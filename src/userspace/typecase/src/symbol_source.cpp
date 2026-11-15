/* symbol_source.cpp — BoxOS's own characters, worked out for the cell. */

#include "typecase/symbol_source.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "boxos_mascot.h"
#include "boxos_symbols.h"
#include "cell_ink.h"

namespace typecase {

namespace {

/* The thinnest line round the eye worth drawing, in pixels; the least the
 * paper round the medium bubble is wider than the bubble, and the least the
 * spark's radius, in pixels. */
constexpr float FinestLine = 0.9f;
constexpr float LeastGap   = 0.75f;
constexpr float LeastSpark = 0.5f;

/* The mark's ink — its body and its bubbles — fitted to a pair of cells
 * Width wide and Height tall, as large as it goes, in the middle: where a
 * point of the master falls in the pair's pixels. */
struct Fit {
    float Scale, Left, Top;

    Outline Disc(const BoxDuppoDisc &Of, float Grow = 0) const
    {
        const float Reach = Of.Radius * Scale + Grow;
        return Oval(Left + Of.X * Scale, Top + Of.Y * Scale, Reach, Reach);
    }
};

Fit FitPair(std::int32_t Width, std::int32_t Height)
{
    const BoxDuppoDisc Inked[] = {BoxDuppoBody, BoxDuppoMarkMoon, BoxDuppoMoons[1], BoxDuppoMoons[2]};
    float Left = Inked[0].X, Top = Inked[0].Y, Right = Left, Bottom = Top;
    for (const BoxDuppoDisc &Each : Inked) {
        Left = std::min(Left, Each.X - Each.Radius), Right = std::max(Right, Each.X + Each.Radius);
        Top = std::min(Top, Each.Y - Each.Radius), Bottom = std::max(Bottom, Each.Y + Each.Radius);
    }
    const float Across = static_cast<float>(2 * Width), Down = static_cast<float>(Height);
    const float Scale  = std::min(Across / (Right - Left), Down / (Bottom - Top));
    return Fit{Scale, (Across - (Right - Left) * Scale) / 2 - Left * Scale, (Down - (Bottom - Top) * Scale) / 2 - Top * Scale};
}

/* Duppo's mark across the pair, laid as the mark is: body, socket, white,
 * pupil, the paper round the medium bubble, the bubbles, the spark. */
CellInk Mark(std::int32_t Width, std::int32_t Height, Rasterizer &Pen)
{
    const Fit At = FitPair(Width, Height);
    CellInk   Ink(2 * Width, Height);
    Ink.Lay(At.Disc(BoxDuppoBody), Pen, true);
    if ((BoxDuppoSocket.Radius - BoxDuppoWhite.Radius) * At.Scale >= FinestLine) {
        Ink.Lay(At.Disc(BoxDuppoSocket), Pen, false);
        Ink.Lay(At.Disc(BoxDuppoWhite), Pen, true);
    }
    Ink.Lay(Oval(At.Left + BoxDuppoPupil.X * At.Scale, At.Top + BoxDuppoPupil.Y * At.Scale,
                 BoxDuppoPupil.ReachX * At.Scale, BoxDuppoPupil.ReachY * At.Scale),
            Pen, false);
    const float Gap = (BoxDuppoMarkGap.Radius - BoxDuppoMarkMoon.Radius) * At.Scale;
    Ink.Lay(At.Disc(BoxDuppoMarkGap, std::max(0.0f, LeastGap - Gap)), Pen, false);
    Ink.Lay(At.Disc(BoxDuppoMarkMoon), Pen, true);
    Ink.Lay(At.Disc(BoxDuppoMoons[1]), Pen, true);
    Ink.Lay(At.Disc(BoxDuppoMoons[2]), Pen, true);
    Ink.Lay(At.Disc(BoxDuppoMarkSpark, std::max(0.0f, LeastSpark - BoxDuppoMarkSpark.Radius * At.Scale)), Pen, true);
    return Ink;
}

/* One cell of the pair, from its column From: a picture of the whole cell. */
GlyphImage Half(CellInk &Ink, std::int32_t From, const Size &At)
{
    GlyphImage Image;
    Image.Top     = -At.Baseline;
    Image.Width   = At.Width;
    Image.Height  = At.Height;
    Image.Advance = static_cast<float>(At.Width);
    Image.Coverage.resize(static_cast<std::size_t>(At.Width) * static_cast<std::size_t>(At.Height));
    for (std::int32_t Y = 0; Y < At.Height; Y++)
        for (std::int32_t X = 0; X < At.Width; X++)
            Image.Coverage[static_cast<std::size_t>(Y * At.Width + X)] = Ink.At(From + X, Y);
    return Image;
}

}  // namespace

bool SymbolSource::Has(char32_t Character) const
{
    return Character == BOXOS_SYMBOL_DUPPO_LEFT || Character == BOXOS_SYMBOL_DUPPO_RIGHT;
}

std::optional<GlyphImage> SymbolSource::Draw(char32_t Character, const Size &At)
{
    if (!Has(Character) || At.Width <= 0 || At.Height <= 0) return std::nullopt;
    CellInk Ink = Mark(At.Width, At.Height, Pen);
    return Half(Ink, Character == BOXOS_SYMBOL_DUPPO_LEFT ? 0 : At.Width, At);
}

}  // namespace typecase
