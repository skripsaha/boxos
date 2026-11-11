#include "painter/tui/subcell.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

#include "boxos_font.h"

#include "painter/tui/glyph.h"

namespace painter::tui {

namespace {

constexpr int CellInk = BOXOS_FONT_WIDTH * BOXOS_FONT_HEIGHT;

/* How much of a glyph the font inks, out of CellInk. */
int InkOf(unsigned char Glyph)
{
    int Count = 0;
    for (const unsigned char Row : BoxFontGlyphs[Glyph]) Count += std::popcount(Row);
    return Count;
}

/* A shade by the font's own picture: the share of the cell its foreground
 * takes. The font's shades are not the usual 25, 50 and 75 percent, so the
 * share is read from it and not written here. */
struct Shade {
    char Glyph;
    int  Ink;
};

constexpr std::size_t ShadeCount = 3;

Shade ShadeAt(std::size_t Index)
{
    const unsigned char Code = static_cast<unsigned char>(Glyphs::LightShade) + static_cast<unsigned char>(Index);
    return {static_cast<char>(Code), InkOf(Code)};
}

struct Rgb {
    int Red, Green, Blue;
};

constexpr int All = 0xF;

Rgb Of(core::Color Ink)
{
    return {Ink.Red(), Ink.Green(), Ink.Blue()};
}

/* The squared distance of two colours, weighted as the eye sees green more
 * than red, and red more than blue. */
int Distance(Rgb A, Rgb B)
{
    const int Red = A.Red - B.Red, Green = A.Green - B.Green, Blue = A.Blue - B.Blue;
    return 2 * Red * Red + 4 * Green * Green + 3 * Blue * Blue;
}

Rgb MeanOf(const Quad &Wanted, int Places)
{
    int Red = 0, Green = 0, Blue = 0, Count = 0;
    for (std::size_t Place = 0; Place < Wanted.size(); Place++) {
        if ((Places & (1 << Place)) == 0) continue;
        Red += Wanted[Place].Red();
        Green += Wanted[Place].Green();
        Blue += Wanted[Place].Blue();
        Count++;
    }
    return {(Red + Count / 2) / Count, (Green + Count / 2) / Count, (Blue + Count / 2) / Count};
}

core::Color Ink(Rgb Value)
{
    const auto Byte = [](int Part) { return static_cast<std::uint8_t>(std::clamp(Part, 0, 255)); };
    return core::Color::Rgb(Byte(Value.Red), Byte(Value.Green), Byte(Value.Blue));
}

struct Fit {
    Cell Look;
    int  Error = 0;
};

int ErrorOver(const Quad &Wanted, int Places, Rgb Shown)
{
    int Error = 0;
    for (std::size_t Place = 0; Place < Wanted.size(); Place++)
        if ((Places & (1 << Place)) != 0) Error += Distance(Of(Wanted[Place]), Shown);
    return Error;
}

/* What a group of places is drawn in: their mean, or on a text page, whose
 * colours are the sixteen it has and not their mixes, the colour of the
 * group that lies nearest the mean, the rarer of two such colours in the
 * whole cell when they are as near. */
Rgb Shown(const Quad &Wanted, int Places, bool Pure)
{
    const Rgb Mean = MeanOf(Wanted, Places);
    if (!Pure) return Mean;
    Rgb  Best = Mean;
    int  Nearest = -1, Common = 0;
    for (std::size_t Place = 0; Place < Wanted.size(); Place++) {
        if ((Places & (1 << Place)) == 0) continue;
        const Rgb Here = Of(Wanted[Place]);
        int       Same = 0;
        for (const core::Color Other : Wanted) Same += Other == Wanted[Place] ? 1 : 0;
        const int Far = Distance(Here, Mean);
        if (Nearest < 0 || Far < Nearest || (Far == Nearest && Same < Common)) {
            Best    = Here;
            Nearest = Far;
            Common  = Same;
        }
    }
    return Best;
}

Fit OneColour(const Quad &Wanted, bool Pure)
{
    const Rgb Only = Shown(Wanted, All, Pure);
    return {Cell{' ', Ink(Only), Ink(Only)}, ErrorOver(Wanted, All, Only)};
}

/* The places in Places drawn in the foreground, the rest in the background. */
Fit Split(const Quad &Wanted, int Places, char Glyph, bool Pure)
{
    const Rgb Front = Shown(Wanted, Places, Pure), Back = Shown(Wanted, All & ~Places, Pure);
    return {Cell{Glyph, Ink(Front), Ink(Back)}, ErrorOver(Wanted, Places, Front) + ErrorOver(Wanted, All & ~Places, Back)};
}

/* What a shade costs, a dither being seen however alike its colours: a part
 * of the cell worth a deviation of about fourteen levels in one sub-pixel,
 * and, as its two colours lie apart, as much as twice the share of the lesser
 * of them times that of the greater says. */
constexpr int ShadeFloor = 2000;

int Texture(Rgb Front, Rgb Back, int Ink)
{
    const std::int64_t Less = std::min(Ink, CellInk - Ink);
    return ShadeFloor + static_cast<int>(Distance(Front, Back) * 2 * Less * (CellInk - Less) / (static_cast<std::int64_t>(CellInk) * CellInk));
}

/* The half shade: a diagonal of one colour against the other. */
Fit Checker(const Quad &Wanted, Shade Half, int Places)
{
    const Rgb Front = MeanOf(Wanted, Places), Back = MeanOf(Wanted, All & ~Places);
    const int Error = ErrorOver(Wanted, Places, Front) + ErrorOver(Wanted, All & ~Places, Back) + Texture(Front, Back, Half.Ink);
    return {Cell{Half.Glyph, Ink(Front), Ink(Back)}, Error};
}

/* The colour that, shown Share parts in CellInk of the way from Rest, is Want. */
Rgb Beyond(Rgb Rest, Rgb Want, int Share)
{
    const auto Part = [&](int From, int To) { return From + (To - From) * CellInk / Share; };
    return {Part(Rest.Red, Want.Red), Part(Rest.Green, Want.Green), Part(Rest.Blue, Want.Blue)};
}

Rgb Clamped(Rgb Value)
{
    return {std::clamp(Value.Red, 0, 255), std::clamp(Value.Green, 0, 255), std::clamp(Value.Blue, 0, 255)};
}

/* Dust: the sub-pixel at Lone drawn as the lesser colour of a shade, sprinkled
 * over the three others. A quarter of the cell is that sub-pixel; where the
 * shade inks less than a quarter the colour is taken further out, and what
 * shows of it is then the colour wanted, as far as the colours reach. */
Fit Dust(const Quad &Wanted, Shade Sprinkle, std::size_t Lone)
{
    const bool Majority = Sprinkle.Ink * 2 > CellInk;
    const int  Less     = Majority ? CellInk - Sprinkle.Ink : Sprinkle.Ink;
    const int  Share    = 4 * Less;
    const Rgb  Rest     = MeanOf(Wanted, All & ~(1 << Lone));
    const Rgb  Want     = Of(Wanted[Lone]);
    const Rgb  Dusted   = Clamped(Beyond(Rest, Want, Share));
    const Rgb  Shown    = {Rest.Red + (Dusted.Red - Rest.Red) * Share / CellInk, Rest.Green + (Dusted.Green - Rest.Green) * Share / CellInk,
                           Rest.Blue + (Dusted.Blue - Rest.Blue) * Share / CellInk};
    const int  Error    = ErrorOver(Wanted, All & ~(1 << Lone), Rest) + Distance(Want, Shown) + Texture(Rest, Dusted, Sprinkle.Ink);
    return {Majority ? Cell{Sprinkle.Glyph, Ink(Rest), Ink(Dusted)} : Cell{Sprinkle.Glyph, Ink(Dusted), Ink(Rest)}, Error};
}

void Consider(Fit &Best, const Fit &Candidate)
{
    if (Candidate.Error < Best.Error) Best = Candidate;
}

}  // namespace

Quad QuadOf(const Cell &Shown) noexcept
{
    const core::Color Front = Shown.Foreground, Back = Shown.Background;
    switch (static_cast<unsigned char>(Shown.Glyph)) {
    case 0xDB: return {Front, Front, Front, Front};
    case 0xDF: return {Front, Front, Back, Back};
    case 0xDC: return {Back, Back, Front, Front};
    case 0xDD: return {Front, Back, Front, Back};
    case 0xDE: return {Back, Front, Back, Front};
    case 0xB0:
    case 0xB1:
    case 0xB2: {
        const Shade Dither = ShadeAt(static_cast<unsigned char>(Shown.Glyph) - 0xB0);
        const core::Color Mixed = core::Color::Mix(Back, Front, static_cast<std::uint8_t>(Dither.Ink * 255 / CellInk));
        return {Mixed, Mixed, Mixed, Mixed};
    }
    default: return {Back, Back, Back, Back};
    }
}

Cell ResolveQuad(const Quad &Wanted, GlassKind Glass) noexcept
{
    if (Wanted[1] == Wanted[0] && Wanted[2] == Wanted[0] && Wanted[3] == Wanted[0])
        return Cell{' ', Wanted[0], Wanted[0]};
    const bool Pure = Glass == GlassKind::Text;
    Fit Best = Split(Wanted, (1 << QuadPlace::TopLeft) | (1 << QuadPlace::TopRight), Glyphs::UpperHalf, Pure);
    Consider(Best, Split(Wanted, (1 << QuadPlace::TopLeft) | (1 << QuadPlace::BottomLeft), Glyphs::LeftHalf, Pure));
    Consider(Best, OneColour(Wanted, Pure));
    if (Pure) return Best.Look;

    for (std::size_t Index = 0; Index < ShadeCount; Index++) {
        const Shade One = ShadeAt(Index);
        if (One.Ink * 2 == CellInk) {
            Consider(Best, Checker(Wanted, One, (1 << QuadPlace::TopLeft) | (1 << QuadPlace::BottomRight)));
            continue;
        }
        for (std::size_t Lone = 0; Lone < Wanted.size(); Lone++) Consider(Best, Dust(Wanted, One, Lone));
    }
    return Best.Look;
}

}  // namespace painter::tui
