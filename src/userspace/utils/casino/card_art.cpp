/* card_art.cpp — playing cards drawn from the font and half-block pixels
 * (card_art.h). */

#include "card_art.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>

#include "canvas.h"
#include "ornament.h"
#include "painter/tui/fine_canvas.h"
#include "painter/tui/glyph.h"

namespace casino {

namespace {

namespace tui    = painter::tui;
namespace Glyphs = painter::tui::Glyphs;

// The back, and a card in flight, are described in a space 64 across and 100
// down, whatever the size.
constexpr float SpaceWide = 64.0f;
constexpr float SpaceHigh = 100.0f;
constexpr float Rounding  = 7.0f;
constexpr int   Samples   = 3;

// The corner of a card at rest: five pixels of the glass, in the units of
// the canvas, where a cell is eight across.
constexpr float CornerRadius = 5.0f / 8.0f;

const Color Skin = Color::Rgb(0xE8, 0xB8, 0x90);

constexpr char32_t Head      = U'\u263B';   /* ☻ */
constexpr char32_t FullBlock = U'\u2588';   /* █ */
constexpr char32_t LowerHalf = U'\u2584';   /* ▄ */
constexpr char32_t UpperHalf = U'\u2580';   /* ▀ */
constexpr char32_t Jewel     = U'\u2022';   /* • */

// Where a pip stands: a column from -1 to 1 and a share of the way down.
struct Pip {
    std::int8_t Col;
    float       Row;
};

constexpr Pip Pips2[]  = {{0, 0}, {0, 1}};
constexpr Pip Pips3[]  = {{0, 0}, {0, 0.5f}, {0, 1}};
constexpr Pip Pips4[]  = {{-1, 0}, {1, 0}, {-1, 1}, {1, 1}};
constexpr Pip Pips5[]  = {{-1, 0}, {1, 0}, {0, 0.5f}, {-1, 1}, {1, 1}};
constexpr Pip Pips6[]  = {{-1, 0}, {1, 0}, {-1, 0.5f}, {1, 0.5f}, {-1, 1}, {1, 1}};
constexpr Pip Pips7[]  = {{-1, 0}, {1, 0}, {0, 0.25f}, {-1, 0.5f}, {1, 0.5f}, {-1, 1}, {1, 1}};
constexpr Pip Pips8[]  = {{-1, 0}, {1, 0}, {0, 0.25f}, {-1, 0.5f}, {1, 0.5f}, {0, 0.75f}, {-1, 1}, {1, 1}};
constexpr Pip Pips9[]  = {{-1, 0}, {1, 0}, {-1, 1.0f / 3}, {1, 1.0f / 3}, {0, 0.5f}, {-1, 2.0f / 3}, {1, 2.0f / 3}, {-1, 1}, {1, 1}};
constexpr Pip Pips10[] = {{-1, 0}, {1, 0}, {0, 1.0f / 6}, {-1, 1.0f / 3}, {1, 1.0f / 3}, {-1, 2.0f / 3}, {1, 2.0f / 3}, {0, 5.0f / 6}, {-1, 1}, {1, 1}};
constexpr Pip PipsAce[] = {{0, 0.5f}};

std::span<const Pip> PipsOf(int Rank)
{
    switch (Rank) {
    case 1:  return PipsAce;
    case 2:  return Pips2;
    case 3:  return Pips3;
    case 4:  return Pips4;
    case 5:  return Pips5;
    case 6:  return Pips6;
    case 7:  return Pips7;
    case 8:  return Pips8;
    case 9:  return Pips9;
    default: return Pips10;
    }
}

Color InkOf(const Look &Shown, Suit Of)
{
    return IsRed(Of) ? Shown.Red : Shown.Black;
}

bool InCard(float X, float Y)
{
    const float OverAcross = std::fabs(X - SpaceWide / 2.0f) - (SpaceWide / 2.0f - Rounding);
    const float OverDown   = std::fabs(Y - SpaceHigh / 2.0f) - (SpaceHigh / 2.0f - Rounding);
    const float Beyond     = std::sqrt(std::max(OverAcross, 0.0f) * std::max(OverAcross, 0.0f) + std::max(OverDown, 0.0f) * std::max(OverDown, 0.0f)) +
                             std::min(std::max(OverAcross, OverDown), 0.0f) - Rounding;
    return Beyond <= 0.0f;
}

// The color of the back at a point of the card's space; false outside the card.
bool BackAt(const Look &Shown, float X, float Y, Color &Ink)
{
    if (!InCard(X, Y)) return false;
    constexpr float Margin = 4.0f;
    if (X < Margin || X > SpaceWide - Margin || Y < Margin || Y > SpaceHigh - Margin) {
        Ink = Shown.CardFace;
        return true;
    }
    const bool Lattice = (static_cast<int>(std::floor((X + Y) / 7.0f)) + static_cast<int>(std::floor((X - Y) / 7.0f))) % 2 == 0;
    Ink                = Shade(Shown.CardBack, Lattice ? 1.2f : 0.8f);
    const float Inset  = std::min(std::min(X - Margin, SpaceWide - Margin - X), std::min(Y - Margin, SpaceHigh - Margin - Y));
    if (Inset > 2.5f && Inset < 4.0f) Ink = Shown.CardFace;
    const float Far = ((X - 32.0f) / 14.0f) * ((X - 32.0f) / 14.0f) + ((Y - 50.0f) / 20.0f) * ((Y - 50.0f) / 20.0f);
    if (Far < 1.0f) {
        Ink = Far > 0.76f ? Shown.Gold : Shade(Shown.CardBack, 0.55f);
        if (std::fabs(X - 32.0f) / 8.0f + std::fabs(Y - 50.0f) / 12.0f < 1.0f) Ink = Shown.Gold;
    }
    return true;
}

// Draws what Take puts at each point of the card's space over the box, to the
// fraction of a pixel: nine samples to a pixel.
template <class Sampler>
void Raster(Canvas &Draw, float Left, float Top, float Wide, float High, Sampler Take)
{
    const std::int32_t From = std::max(static_cast<std::int32_t>(std::floor(Left)), 0);
    const std::int32_t Up   = std::max(static_cast<std::int32_t>(std::floor(Top)), 0);
    const std::int32_t To   = std::min(static_cast<std::int32_t>(std::ceil(Left + Wide)), Draw.Wide());
    const std::int32_t Down = std::min(static_cast<std::int32_t>(std::ceil(Top + High)), Draw.High());
    for (std::int32_t Row = Up; Row < Down; Row++) {
        for (std::int32_t Col = From; Col < To; Col++) {
            float Red = 0, Green = 0, Blue = 0;
            int   Hits = 0;
            for (int Down2 = 0; Down2 < Samples; Down2++) {
                for (int Across = 0; Across < Samples; Across++) {
                    const float X = (static_cast<float>(Col) + (static_cast<float>(Across) + 0.5f) / Samples - Left) / Wide * SpaceWide;
                    const float Y = (static_cast<float>(Row) + (static_cast<float>(Down2) + 0.5f) / Samples - Top) / High * SpaceHigh;
                    Color       Ink;
                    if (X < 0 || Y < 0 || X >= SpaceWide || Y >= SpaceHigh || !Take(X, Y, Ink)) continue;
                    Red += Ink.Red();
                    Green += Ink.Green();
                    Blue += Ink.Blue();
                    Hits++;
                }
            }
            if (Hits == 0) continue;
            const auto Mean = [Hits](float Sum) { return static_cast<std::uint8_t>(Sum / static_cast<float>(Hits) + 0.5f); };
            Draw.Plot(Col, Row, Color::Rgb(Mean(Red), Mean(Green), Mean(Blue)), static_cast<float>(Hits) / (Samples * Samples));
        }
    }
}

// A card seen as pixels: its back, or a blank face while it turns.
void Body(Canvas &Pixels, const Look &Shown, bool Back, float Left, float Top, float Wide, float High)
{
    if (Pixels.Smooth()) {
        const float Round = std::max(1.0f, Wide * Rounding / SpaceWide);
        Pixels.Rounded(Left + 0.5f, Top + 0.8f, Wide + 0.9f, High + 0.9f, Round + 0.8f, painter::core::Colors::Black, 0.16f);
        Pixels.Rounded(Left + 1.0f, Top + 1.0f, Wide, High, Round, painter::core::Colors::Black, 0.3f);
    }
    if (Back) Raster(Pixels, Left, Top, Wide, High, [&](float X, float Y, Color &Ink) { return BackAt(Shown, X, Y, Ink); });
    else Raster(Pixels, Left, Top, Wide, High, [&](float X, float Y, Color &Ink) {
        Ink = Shown.CardFace;
        return InCard(X, Y);
    });
}

void Put(tui::Frame &Out, std::int32_t Col, std::int32_t Row, char32_t Glyph, Color Ink, Color Paper)
{
    Out.Put({Col, Row}, Glyph, tui::TextStyle{Ink, Paper});
}

// The shadow a card casts to the right and below, as thin as a half cell.
void Shadow(tui::Frame &Out, const Look &Shown, painter::core::Point At, CardSize Size)
{
    const auto Cast = [&](std::int32_t Col, std::int32_t Row, char32_t Glyph) {
        const tui::Cell Beneath = Out.Read({Col, Row});
        if (Beneath.Background == Shown.CardFace || Beneath.Glyph != ' ') return;
        Put(Out, Col, Row, Glyph, Shade(Beneath.Background, 0.45f), Beneath.Background);
    };
    for (std::int32_t Row = At.Y + 1; Row <= At.Y + Size.Rows; Row++) Cast(At.X + Size.Cols, Row, Glyphs::LeftHalf);
    for (std::int32_t Col = At.X + 1; Col < At.X + Size.Cols; Col++) Cast(Col, At.Y + Size.Rows, UpperHalf);
}

// The rank over the suit in the top left corner, and under it in the bottom
// right where the card is read upside down.
void Indices(tui::Frame &Out, const Look &Shown, Card One, painter::core::Point At, CardSize Size)
{
    const Color            Ink  = InkOf(Shown, One.Of);
    const std::string_view Rank = RankName(One.Rank);
    const auto             Len  = static_cast<std::int32_t>(Rank.size());
    const std::int32_t     Mark = At.X + Size.Cols - 2;
    Out.Write({At.X + 1, At.Y}, Rank, tui::TextStyle{Ink, Shown.CardFace});
    Put(Out, At.X + 1, At.Y + 1, SuitGlyph(One.Of), Ink, Shown.CardFace);
    Out.Write({Mark - Len + 1, At.Y + Size.Rows - 1}, Rank, tui::TextStyle{Ink, Shown.CardFace});
    Put(Out, Mark, At.Y + Size.Rows - 2, SuitGlyph(One.Of), Ink, Shown.CardFace);
}

// A court card on a small card: a plaque of rules with the letter and the
// suit between.
void Plaque(tui::Frame &Out, const Look &Shown, Card One, painter::core::Point At, CardSize Size)
{
    const Color        Ink    = InkOf(Shown, One.Of);
    const std::int32_t Middle = At.X + Size.Cols / 2;
    const char         Letter = RankName(One.Rank)[0];
    const std::int32_t Top = At.Y + 1, Bottom = At.Y + Size.Rows - 2;
    Put(Out, Middle - 1, Top, Glyphs::CornerTL, Ink, Shown.CardFace);
    Put(Out, Middle, Top, Glyphs::RuleAcross, Ink, Shown.CardFace);
    Put(Out, Middle + 1, Top, Glyphs::CornerTR, Ink, Shown.CardFace);
    for (std::int32_t Row = Top + 1; Row < Bottom; Row++) {
        const bool Edge = Row == Top + 1 || Row == Bottom - 1;
        Put(Out, Middle - 1, Row, Glyphs::RuleDown, Ink, Shown.CardFace);
        Put(Out, Middle + 1, Row, Glyphs::RuleDown, Ink, Shown.CardFace);
        Put(Out, Middle, Row, Edge ? Letter : SuitGlyph(One.Of), Ink, Shown.CardFace);
    }
    Put(Out, Middle - 1, Bottom, Glyphs::CornerBL, Ink, Shown.CardFace);
    Put(Out, Middle, Bottom, Glyphs::RuleAcross, Ink, Shown.CardFace);
    Put(Out, Middle + 1, Bottom, Glyphs::CornerBR, Ink, Shown.CardFace);
}

// The head-dress of a court card, as wide as the portrait allows, and its
// color; Turned for the half that stands on its head.
std::u32string Crown(int Rank, std::int32_t Wide, bool Turned, Color &Ink, const Look &Shown)
{
    std::u32string Full = Rank == 13 ? std::u32string{LowerHalf, UpperHalf, LowerHalf, UpperHalf, LowerHalf}
                          : Rank == 12 ? std::u32string{LowerHalf, Jewel, LowerHalf, Jewel, LowerHalf}
                                       : std::u32string{LowerHalf, LowerHalf, LowerHalf, LowerHalf, LowerHalf};
    Ink = Rank == 11 ? Shown.Black : Shown.Gold;
    const auto Keep = std::min<std::size_t>(static_cast<std::size_t>(Wide), Full.size());
    std::u32string Cut = Full.substr((Full.size() - Keep) / 2, Keep);
    if (Turned)
        for (char32_t &Glyph : Cut) Glyph = Glyph == LowerHalf ? UpperHalf : Glyph == UpperHalf ? LowerHalf : Glyph;
    return Cut;
}

// A court card as a framed portrait: a crowned head and shoulders, turned
// round for the lower half, the suit on the line between.
void Portrait(tui::Frame &Out, const Look &Shown, Card One, painter::core::Point At, CardSize Size)
{
    const Color        Ink    = InkOf(Shown, One.Of);
    const std::int32_t Left   = At.X + 3, Wide = Size.Cols - 6, Top = At.Y + 1, High = Size.Rows - 2;
    const std::int32_t Middle = Left + Wide / 2;
    const tui::TextStyle Rule{Ink, Shown.CardFace};
    Out.Put({Left, Top}, Glyphs::CornerTL, Rule);
    Out.Put({Left + Wide - 1, Top}, Glyphs::CornerTR, Rule);
    Out.Put({Left, Top + High - 1}, Glyphs::CornerBL, Rule);
    Out.Put({Left + Wide - 1, Top + High - 1}, Glyphs::CornerBR, Rule);
    for (std::int32_t Col = Left + 1; Col < Left + Wide - 1; Col++) {
        Out.Put({Col, Top}, Glyphs::RuleAcross, Rule);
        Out.Put({Col, Top + High - 1}, Glyphs::RuleAcross, Rule);
    }
    for (std::int32_t Row = Top + 1; Row < Top + High - 1; Row++) {
        Out.Put({Left, Row}, Glyphs::RuleDown, Rule);
        Out.Put({Left + Wide - 1, Row}, Glyphs::RuleDown, Rule);
    }
    const std::int32_t Inner = Wide - 2, Half = (High - 3) / 2, Line = Top + 1 + Half;
    for (std::int32_t Col = Left + 1; Col < Left + Wide - 1; Col++) Out.Put({Col, Line}, Glyphs::RuleAcross, Rule);
    Out.Put({Middle, Line}, SuitGlyph(One.Of), Rule);
    for (int Turned = 0; Turned < 2; Turned++) {
        const std::int32_t   Dir = Turned ? 1 : -1, Start = Turned ? Line + 1 : Line - 1;
        std::int32_t         Row = Start + Dir * (Half - 1);
        Color                CrownInk;
        const std::u32string Hat = Crown(One.Rank, Inner, Turned != 0, CrownInk, Shown);
        const auto           Width = static_cast<std::int32_t>(Hat.size());
        for (std::int32_t At2 = 0; At2 < Width; At2++) Put(Out, Middle - Width / 2 + At2, Row, Hat[static_cast<std::size_t>(At2)], CrownInk, Shown.CardFace);
        Row -= Dir;
        if (Half >= 2) Put(Out, Middle, Row, Head, Skin, Shown.CardFace);
        for (std::int32_t Down = 2; Down < Half; Down++) {
            Row -= Dir;
            const std::int32_t Reach = std::min(Inner, 3 + 2 * (Down - 2)) / 2;
            for (std::int32_t Off = -Reach; Off <= Reach; Off++)
                Put(Out, Middle + Off, Row, FullBlock, Ink, Shown.CardFace);
        }
    }
}

// Pips as the font's suit mark, laid on the cells in the classic arrangement.
void Pips(tui::Frame &Out, const Look &Shown, Card One, painter::core::Point At, CardSize Size)
{
    const Color        Ink    = InkOf(Shown, One.Of);
    const std::int32_t Middle = At.X + Size.Cols / 2;
    const std::int32_t Gap    = Size.Cols >= 11 ? 3 : 2;
    for (const Pip &Spot : PipsOf(One.Rank))
        Put(Out, Middle + Spot.Col * Gap, At.Y + 1 + static_cast<std::int32_t>(Spot.Row * static_cast<float>(Size.Rows - 3) + 0.5f), SuitGlyph(One.Of), Ink,
            Shown.CardFace);
}

}  // namespace

CardSize CardSizeFor(std::int32_t GlassRows)
{
    if (GlassRows < 34) return CardSize{9, 7};
    if (GlassRows < 46) return CardSize{11, 9};
    if (GlassRows < 60) return CardSize{13, 11};
    return CardSize{15, 13};
}

void PaintCard(tui::Frame &Out, const Look &Shown, const Card *Face, painter::core::Point At, CardSize Size)
{
    if (!Face) {
        Canvas Pixels(Out, Hard(Out));
        Body(Pixels, Shown, true, static_cast<float>(At.X), static_cast<float>(At.Y * 2), static_cast<float>(Size.Wide()), static_cast<float>(Size.High()));
        return;
    }
    if (!Hard(Out)) Shadow(Out, Shown, At, Size);
    tui::FineCanvas(Out).Rounded(static_cast<float>(At.X), static_cast<float>(At.Y * 2), static_cast<float>(Size.Cols),
                                 static_cast<float>(Size.Rows * 2), CornerRadius, Shown.CardFace);
    if (IsCourt(*Face)) {
        if (Size.Rows >= 9) Portrait(Out, Shown, *Face, At, Size);
        else Plaque(Out, Shown, *Face, At, Size);
    } else {
        Pips(Out, Shown, *Face, At, Size);
    }
    Indices(Out, Shown, *Face, At, Size);
}

void PaintMoving(tui::Frame &Out, const Look &Shown, const Card *Face, float Across, float Down, CardSize Size, float Squeeze)
{
    Canvas      Pixels(Out, Hard(Out));
    const float Wide = std::max(static_cast<float>(Size.Wide()) * Squeeze, 0.5f);
    Body(Pixels, Shown, Face == nullptr, Across + (static_cast<float>(Size.Wide()) - Wide) / 2.0f, Down, Wide, static_cast<float>(Size.High()));
}

}  // namespace casino
