#include "painter/tui/mosaic.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

#include "boxos_font.h"
#include "boxos_glyph_fold.h"
#include "boxos_mosaic.h"

#include "painter/tui/glyph.h"

namespace painter::tui {

namespace {

constexpr std::size_t TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3;

constexpr int CellInk = BOXOS_FONT_WIDTH * BOXOS_FONT_HEIGHT;

/* How much of the cell the display's font inks to show Character, out of
 * CellInk: the glyph it draws it with, read from the same font. */
int InkOf(char32_t Character)
{
    int Count = 0;
    for (const unsigned char Row : BoxFontGlyphs[BoxGlyphFoldOne(Character)]) Count += std::popcount(Row);
    return Count;
}

/* A shade by the font's own picture: the share of the cell its foreground
 * takes. The font's shades are not the usual 25, 50 and 75 percent, so the
 * share is read from it and not written here. */
struct Shade {
    char32_t Glyph;
    int      Ink;
};

constexpr std::array<char32_t, 3> Shades = {Glyphs::LightShade, Glyphs::MidShade, Glyphs::DenseShade};
constexpr std::size_t             ShadeCount = Shades.size();

Shade ShadeAt(std::size_t Index)
{
    return {Shades[Index], InkOf(Shades[Index])};
}

struct Rgb {
    int Red, Green, Blue;
};

int All(const Mosaic &Wanted)
{
    return (1 << Wanted.Count()) - 1;
}

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

/* The share of a sub-pixel's colour that is seen, of 255. */
int OpacityOf(core::Color Ink)
{
    return Ink.Opacity();
}

constexpr int Whole = 255;

/* How much a difference of opacity is worth against one of colour: a
 * sub-pixel shown see-through where it is whole is as far off as a colour
 * about a third of the way across the cube. */
constexpr int OpacityWeight = 6;

/* The mean of a group of places weighted by how opaque each is: a
 * see-through place has no colour to give. There must be one that is seen. */
Rgb MeanOf(const Mosaic &Wanted, int Places)
{
    int Red = 0, Green = 0, Blue = 0, Seen = 0;
    for (std::size_t Place = 0; Place < Wanted.Count(); Place++) {
        if ((Places & (1 << Place)) == 0) continue;
        const int Share = OpacityOf(Wanted.At[Place]);
        Red += Wanted.At[Place].Red() * Share;
        Green += Wanted.At[Place].Green() * Share;
        Blue += Wanted.At[Place].Blue() * Share;
        Seen += Share;
    }
    return {(Red + Seen / 2) / Seen, (Green + Seen / 2) / Seen, (Blue + Seen / 2) / Seen};
}

core::Color Ink(Rgb Value, int Opacity)
{
    const auto Byte = [](int Part) { return static_cast<std::uint8_t>(std::clamp(Part, 0, 255)); };
    return core::Color::Rgb(Byte(Value.Red), Byte(Value.Green), Byte(Value.Blue)).WithOpacity(static_cast<std::uint8_t>(Opacity));
}

/* A cell chosen for a cell's sub-pixels, and its error against them. */
struct Fit {
    Cell Look;
    int  Error = 0;
};

/* What a group of places shown in Shown at Opacity is off by: for each, its
 * colour's distance as far as both are seen, and its opacity's difference. */
int ErrorOver(const Mosaic &Wanted, int Places, Rgb Shown, int Opacity)
{
    int Error = 0;
    for (std::size_t Place = 0; Place < Wanted.Count(); Place++) {
        if ((Places & (1 << Place)) == 0) continue;
        const int Wants = OpacityOf(Wanted.At[Place]);
        Error += Distance(Of(Wanted.At[Place]), Shown) * std::min(Wants, Opacity) / Whole + OpacityWeight * (Wants - Opacity) * (Wants - Opacity);
    }
    return Error;
}

/* What a group of places is drawn in: their mean, or on a text page, whose
 * colours are the sixteen it has and not their mixes, the colour of the
 * group that lies nearest the mean, the rarer of two such colours in the
 * whole cell when they are as near. */
Rgb Shown(const Mosaic &Wanted, int Places, bool Pure)
{
    const Rgb Mean = MeanOf(Wanted, Places);
    if (!Pure) return Mean;
    Rgb  Best = Mean;
    int  Nearest = -1, Common = 0;
    for (std::size_t Place = 0; Place < Wanted.Count(); Place++) {
        if ((Places & (1 << Place)) == 0 || OpacityOf(Wanted.At[Place]) == 0) continue;
        const Rgb Here = Of(Wanted.At[Place]);
        int       Same = 0;
        for (std::size_t Other = 0; Other < Wanted.Count(); Other++) Same += Wanted.At[Other] == Wanted.At[Place] ? 1 : 0;
        const int Far = Distance(Here, Mean);
        if (Nearest < 0 || Far < Nearest || (Far == Nearest && Same < Common)) {
            Best    = Here;
            Nearest = Far;
            Common  = Same;
        }
    }
    return Best;
}

/* A group of places drawn in one colour at the mean of their opacities, or
 * see-through when none of them is seen at all. */
struct Side {
    core::Color Colour;
    int         Error = 0;
};

Side SideOf(const Mosaic &Wanted, int Places, bool Pure)
{
    int Seen = 0, Count = 0;
    for (std::size_t Place = 0; Place < Wanted.Count(); Place++) {
        if ((Places & (1 << Place)) == 0) continue;
        Seen += OpacityOf(Wanted.At[Place]);
        Count++;
    }
    if (Seen == 0) return {core::Color::Through()};
    const int Opacity = (Seen + Count / 2) / Count;
    const Rgb Colour  = Shown(Wanted, Places, Pure);
    return {Ink(Colour, Opacity), ErrorOver(Wanted, Places, Colour, Opacity)};
}

Fit OneColour(const Mosaic &Wanted, bool Pure)
{
    const Side Only = SideOf(Wanted, All(Wanted), Pure);
    return {Cell{' ', Only.Colour, Only.Colour}, Only.Error};
}

/* The places in Places drawn in the foreground, the rest in the background. */
Fit Split(const Mosaic &Wanted, int Places, char32_t Glyph, bool Pure)
{
    const Side Front = SideOf(Wanted, Places, Pure), Back = SideOf(Wanted, All(Wanted) & ~Places, Pure);
    return {Cell{Glyph, Front.Colour, Back.Colour}, Front.Error + Back.Error};
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
Fit Checker(const Mosaic &Wanted, Shade Half, int Places)
{
    const Rgb Front = MeanOf(Wanted, Places), Back = MeanOf(Wanted, All(Wanted) & ~Places);
    const int Error = ErrorOver(Wanted, Places, Front, Whole) + ErrorOver(Wanted, All(Wanted) & ~Places, Back, Whole) + Texture(Front, Back, Half.Ink);
    return {Cell{Half.Glyph, Ink(Front, Whole), Ink(Back, Whole)}, Error};
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
Fit Dust(const Mosaic &Wanted, Shade Sprinkle, std::size_t Lone)
{
    const bool Majority = Sprinkle.Ink * 2 > CellInk;
    const int  Less     = Majority ? CellInk - Sprinkle.Ink : Sprinkle.Ink;
    const int  Share    = 4 * Less;
    const Rgb  Rest     = MeanOf(Wanted, All(Wanted) & ~(1 << Lone));
    const Rgb  Want     = Of(Wanted.At[Lone]);
    const Rgb  Dusted   = Clamped(Beyond(Rest, Want, Share));
    const Rgb  Shown    = {Rest.Red + (Dusted.Red - Rest.Red) * Share / CellInk, Rest.Green + (Dusted.Green - Rest.Green) * Share / CellInk,
                           Rest.Blue + (Dusted.Blue - Rest.Blue) * Share / CellInk};
    const int  Error    = ErrorOver(Wanted, All(Wanted) & ~(1 << Lone), Rest, Whole) + Distance(Want, Shown) + Texture(Rest, Dusted, Sprinkle.Ink);
    return {Majority ? Cell{Sprinkle.Glyph, Ink(Rest, Whole), Ink(Dusted, Whole)} : Cell{Sprinkle.Glyph, Ink(Dusted, Whole), Ink(Rest, Whole)}, Error};
}

void Consider(Fit &Best, const Fit &Candidate)
{
    if (Candidate.Error < Best.Error) Best = Candidate;
}

bool AllWhole(const Mosaic &Wanted)
{
    return std::all_of(Wanted.At.begin(), Wanted.At.begin() + Wanted.Count(), [](core::Color Place) { return OpacityOf(Place) == Whole; });
}

bool Uniform(const Mosaic &Wanted)
{
    return std::all_of(Wanted.At.begin() + 1, Wanted.At.begin() + Wanted.Count(), [&](core::Color Place) { return Place == Wanted.At[0]; });
}

/* The glass draws the grid of Rows exactly when its display said so. */
bool Draws(std::uint8_t Mosaics, std::uint8_t Rows)
{
    switch (Rows) {
    case BOXOS_MOSAIC_QUADRANTS_ROWS: return (Mosaics & DISPLAY_MOSAIC_QUADRANTS) != 0;
    case BOXOS_MOSAIC_SEXTANTS_ROWS:  return (Mosaics & DISPLAY_MOSAIC_SEXTANTS) != 0;
    case BOXOS_MOSAIC_OCTANTS_ROWS:   return (Mosaics & DISPLAY_MOSAIC_OCTANTS) != 0;
    default:                          return false;
    }
}

const std::uint32_t *CharactersOf(std::uint8_t Rows)
{
    switch (Rows) {
    case BOXOS_MOSAIC_QUADRANTS_ROWS: return BoxMosaicQuadrants;
    case BOXOS_MOSAIC_SEXTANTS_ROWS:  return BoxMosaicSextants;
    default:                          return BoxMosaicOctants;
    }
}

/* How much of the row Target of TargetRows the row Source of Rows covers, the
 * cell being cut into Rows times TargetRows units of height: a row of either
 * grid is as many units as the other has rows. */
int Overlap(int Source, int Rows, int Target, int TargetRows)
{
    return std::max(0, std::min((Source + 1) * TargetRows, (Target + 1) * Rows) - std::max(Source * TargetRows, Target * Rows));
}

/* The two-colour cell that is the best fit, over every split of the
 * sub-pixels into a foreground and its complement. The last sub-pixel is
 * never in the foreground, which names each picture once; one colour is tried
 * first, so it wins a tie. */
Cell ExactFit(const Mosaic &Wanted)
{
    const std::uint32_t *Characters = CharactersOf(Wanted.Rows);
    Fit Best = OneColour(Wanted, false);
    for (int Places = 1; Places < (1 << (Wanted.Count() - 1)); Places++)
        Consider(Best, Split(Wanted, Places, Characters[Places], false));
    return Best.Look;
}

/* Two rows from more: each of the two takes the means of the rows it
 * overlaps, weighted by the area of the overlap and by how opaque each
 * sub-pixel is, at the mean of their opacities; a row nothing seen lies in
 * is see-through. */
Mosaic Halved(const Mosaic &Wanted)
{
    Mosaic Folded;
    for (int Target = 0; Target < 2; Target++) {
        for (int Column = 0; Column < 2; Column++) {
            int Red = 0, Green = 0, Blue = 0, Seen = 0, Weights = 0;
            for (int Source = 0; Source < Wanted.Rows; Source++) {
                const int Weight = Overlap(Source, Wanted.Rows, Target, 2);
                if (Weight == 0) continue;
                const core::Color Part = Wanted.At[static_cast<std::size_t>(Source * 2 + Column)];
                const int Share = Weight * OpacityOf(Part);
                Red += Part.Red() * Share;
                Green += Part.Green() * Share;
                Blue += Part.Blue() * Share;
                Seen += Share;
                Weights += Weight;
            }
            core::Color &Place = Folded.At[static_cast<std::size_t>(Target * 2 + Column)];
            if (Seen == 0) {
                Place = core::Color::Through();
                continue;
            }
            Place = Ink({(Red + Seen / 2) / Seen, (Green + Seen / 2) / Seen, (Blue + Seen / 2) / Seen}, (Seen + Weights / 2) / Weights);
        }
    }
    return Folded;
}

/* The cell on two rows: the legacy candidates, none of which is a quarter. */
Cell ResolveHalves(const Mosaic &Wanted, GlassKind Glass)
{
    const bool Pure = Glass == GlassKind::Text;
    Fit Best = Split(Wanted, (1 << TopLeft) | (1 << TopRight), Glyphs::UpperHalf, Pure);
    Consider(Best, Split(Wanted, (1 << TopLeft) | (1 << BottomLeft), Glyphs::LeftHalf, Pure));
    Consider(Best, OneColour(Wanted, Pure));
    if (Pure || !AllWhole(Wanted)) return Best.Look;

    for (std::size_t Index = 0; Index < ShadeCount; Index++) {
        const Shade One = ShadeAt(Index);
        if (One.Ink * 2 == CellInk) {
            Consider(Best, Checker(Wanted, One, (1 << TopLeft) | (1 << BottomRight)));
            continue;
        }
        for (std::size_t Lone = 0; Lone < Wanted.Count(); Lone++) Consider(Best, Dust(Wanted, One, Lone));
    }
    return Best.Look;
}

}  // namespace

Mosaic MosaicOf(const Cell &Shown, std::uint8_t Rows) noexcept
{
    Mosaic Seen;
    Seen.Rows = Rows;
    const core::Color Front = Shown.Foreground, Back = Shown.Background;
    switch (Shown.Glyph) {
    case Glyphs::LightShade:
    case Glyphs::MidShade:
    case Glyphs::DenseShade: {
        const Shade Dither = ShadeAt(static_cast<std::size_t>(Shown.Glyph - Glyphs::LightShade));
        Seen.At.fill(core::Color::Mix(Back, Front, static_cast<std::uint8_t>(Dither.Ink * 255 / CellInk)));
        return Seen;
    }
    default: break;
    }

    std::uint8_t Grid = 0, Pattern = 0;
    if (!BoxMosaicShapeOf(Shown.Glyph, &Grid, &Pattern)) {
        Seen.At.fill(Back);
        return Seen;
    }
    for (int Target = 0; Target < Rows; Target++) {
        for (int Column = 0; Column < 2; Column++) {
            int Covered = 0;
            for (int Source = 0; Source < Grid; Source++)
                if ((Pattern & (1 << (Source * 2 + Column))) != 0) Covered += Overlap(Source, Grid, Target, Rows);
            core::Color &Place = Seen.At[static_cast<std::size_t>(Target * 2 + Column)];
            if (Covered == 0) Place = Back;
            else if (Covered == Grid) Place = Front;
            else Place = core::Color::Mix(Back, Front, static_cast<std::uint8_t>((Covered * 255 + Grid / 2) / Grid));
        }
    }
    return Seen;
}

Cell ResolveMosaic(const Mosaic &Wanted, GlassKind Glass, std::uint8_t Mosaics) noexcept
{
    if (Uniform(Wanted)) return Cell{' ', Wanted.At[0], Wanted.At[0]};
    if (Glass == GlassKind::Pixels && Draws(Mosaics, Wanted.Rows)) return ExactFit(Wanted);
    return ResolveHalves(Wanted.Rows == 2 ? Wanted : Halved(Wanted), Glass);
}

}  // namespace painter::tui
