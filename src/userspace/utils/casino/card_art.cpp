/* card_art.cpp — playing cards drawn from shapes (card_art.h). */

#include "card_art.h"

#include <algorithm>
#include <cmath>
#include <span>

#include "canvas.h"
#include "painter/tui/glyph.h"
#include "suit_shape.h"

namespace casino {

namespace {

namespace tui    = painter::tui;
namespace Glyphs = painter::tui::Glyphs;

// A card is described in a space 64 across and 100 down, whatever its size.
constexpr float SpaceWide = 64.0f;
constexpr float SpaceHigh = 100.0f;
constexpr float Rounding  = 7.0f;
constexpr int   Samples   = 3;

// Where the pips and the court figures go.
constexpr float PipMiddle = 32.0f;
constexpr float PipStep   = 15.0f;
constexpr float PipTop    = 25.0f;
constexpr float PipSpan   = 50.0f;
constexpr float PipRadius = 8.0f;
constexpr float AceRadius = 17.0f;

const Color Skin = Color::Rgb(0xF0, 0xC8, 0xA0);

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

bool InEllipse(float A, float B, float CenterA, float CenterB, float ReachA, float ReachB)
{
    const float Over = (A - CenterA) / ReachA, Under = (B - CenterB) / ReachB;
    return Over * Over + Under * Under <= 1.0f;
}

// The upper half of a court figure, seen from the middle of the panel up: A
// across from -1 to 1, B up from 0 to 1. The lower half is the same turned round.
bool Figure(const Look &Shown, Card One, float A, float B, Color &Ink)
{
    const Color Suit  = InkOf(Shown, One.Of);
    const float Mark  = 0.27f;
    const float Across = (A + 0.7f) / Mark, Up = (B - 0.82f) / (Mark * 0.7f);
    if (std::fabs(Across) <= 1.0f && std::fabs(Up) <= 1.0f && InSuit(One.Of, Across, Up)) {
        Ink = Suit;
        return true;
    }
    if (One.Rank == 13) {
        if (B >= 0.8f && B < 0.86f && std::fabs(A) <= 0.36f) {
            Ink = Shown.Gold;
            return true;
        }
        for (const float Peak : {-0.26f, 0.0f, 0.26f})
            if (std::fabs(A - Peak) < 0.1f && B >= 0.86f && B < 0.86f + 0.13f * (1.0f - std::fabs(A - Peak) / 0.1f)) {
                Ink = Shown.Gold;
                return true;
            }
    }
    if (One.Rank == 12) {
        if (B >= 0.8f && B < 0.88f && std::fabs(A) <= 0.36f) {
            Ink = Shown.Gold;
            return true;
        }
        for (const float Ball : {-0.3f, 0.0f, 0.3f})
            if (InEllipse(A, B, Ball, 0.93f, 0.09f, 0.06f)) {
                Ink = Shown.Gold;
                return true;
            }
    }
    if (One.Rank == 11) {
        if (B > 0.74f && InEllipse(A, B, 0.0f, 0.74f, 0.34f, 0.17f)) {
            Ink = Suit;
            return true;
        }
        if (std::fabs(A - 0.22f - (B - 0.86f) * 1.2f) < 0.06f && B > 0.86f && B < 0.99f) {
            Ink = Shown.Gold;
            return true;
        }
    }
    if (InEllipse(A, B, 0.0f, 0.6f, 0.3f, 0.19f)) {
        Ink = Skin;
        return true;
    }
    if (B >= 0.38f && B < 0.46f && std::fabs(A) <= 0.55f) {
        Ink = Shown.CardFace;
        return true;
    }
    if (B < 0.46f && std::fabs(A) <= 0.98f - 0.45f * B) {
        Ink = std::fabs(A) < 0.04f ? Shade(Suit, 0.6f) : B < 0.1f ? Shown.Gold : Suit;
        return true;
    }
    return false;
}

// The court card's panel and the figure in it, the lower half turned round.
bool Court(const Look &Shown, Card One, float X, float Y, Color &Ink)
{
    constexpr float Left = 11.0f, Right = 53.0f, Top = 20.0f, Bottom = 80.0f;
    if (X < Left || X >= Right || Y < Top || Y >= Bottom) return false;
    if (std::min(std::min(X - Left, Right - X), std::min(Y - Top, Bottom - Y)) < 1.4f) {
        Ink = InkOf(Shown, One.Of);
        return true;
    }
    float A = (X - 32.0f) / 21.0f, B = (50.0f - Y) / 30.0f;
    if (B < 0.0f) {
        A = -A;
        B = -B;
    }
    Ink = Shade(Shown.CardFace, 0.9f);
    Figure(Shown, One, A, B, Ink);
    return true;
}

// The color of a face at a point of the card's space; false outside the card.
bool FaceAt(const Look &Shown, Card One, bool Pips, float X, float Y, Color &Ink)
{
    if (!InCard(X, Y)) return false;
    Ink = Shown.CardFace;
    if (!Pips) return true;
    if (IsCourt(One)) {
        Court(Shown, One, X, Y, Ink);
        return true;
    }
    for (const Pip &Spot : PipsOf(One.Rank)) {
        const float Radius = One.Rank == 1 ? AceRadius : PipRadius;
        const float CenterY = PipTop + Spot.Row * PipSpan;
        const float U = (X - (PipMiddle + Spot.Col * PipStep)) / Radius;
        float       V = (CenterY - Y) / Radius;
        if (Spot.Row > 0.5f) V = -V;
        if (std::fabs(U) <= 1.0f && std::fabs(V) <= 1.0f && InSuit(One.Of, U, V)) {
            Ink = InkOf(Shown, One.Of);
            return true;
        }
    }
    return true;
}

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

void Shadow(Canvas &Draw, float Left, float Top, float Wide, float High)
{
    if (!Draw.Smooth()) return;
    const float Round = std::max(1.0f, Wide * Rounding / SpaceWide);
    Draw.Rounded(Left + 0.5f, Top + 0.8f, Wide + 0.9f, High + 0.9f, Round + 0.8f, painter::core::Colors::Black, 0.16f);
    Draw.Rounded(Left + 1.0f, Top + 1.0f, Wide, High, Round, painter::core::Colors::Black, 0.3f);
}

void Body(Canvas &Pixels, const Look &Shown, const Card *Face, bool Pips, float Left, float Top, float Wide, float High)
{
    Shadow(Pixels, Left, Top, Wide, High);
    if (Face) Raster(Pixels, Left, Top, Wide, High, [&](float X, float Y, Color &Ink) { return FaceAt(Shown, *Face, Pips, X, Y, Ink); });
    else Raster(Pixels, Left, Top, Wide, High, [&](float X, float Y, Color &Ink) { return BackAt(Shown, X, Y, Ink); });
}

void Put(tui::Frame &Out, std::int32_t Col, std::int32_t Row, char Glyph, Color Ink, Color Face)
{
    Out.Put({Col, Row}, Glyph, tui::TextStyle{Ink, Face});
}

void Write(tui::Frame &Out, std::int32_t Col, std::int32_t Row, std::string_view Words, Color Ink, Color Face)
{
    Out.Write({Col, Row}, Words, tui::TextStyle{Ink, Face});
}

// The rank over the suit in the top left corner, and under it in the bottom
// right where the card is read upside down.
void Indices(tui::Frame &Out, const Look &Shown, Card One, painter::core::Point At, CardSize Size)
{
    const Color        Ink  = InkOf(Shown, One.Of);
    const std::string_view Rank = RankName(One.Rank);
    const auto         Len  = static_cast<std::int32_t>(Rank.size());
    const std::int32_t Mark = At.X + Size.Cols - 2;
    Write(Out, At.X + 1, At.Y, Rank, Ink, Shown.CardFace);
    Put(Out, At.X + 1, At.Y + 1, SuitGlyph(One.Of), Ink, Shown.CardFace);
    Write(Out, Mark - Len + 1, At.Y + Size.Rows - 1, Rank, Ink, Shown.CardFace);
    Put(Out, Mark, At.Y + Size.Rows - 2, SuitGlyph(One.Of), Ink, Shown.CardFace);
}

// A small card has no room for drawn pips: the suit's mark in the font stands
// for each, in the same places, and a court card has a plaque.
void GlyphPips(tui::Frame &Out, const Look &Shown, Card One, painter::core::Point At, CardSize Size)
{
    const Color        Ink    = InkOf(Shown, One.Of);
    const char         Mark   = SuitGlyph(One.Of);
    const std::int32_t Middle = At.X + Size.Cols / 2;
    if (IsCourt(One)) {
        const char         Letter = RankName(One.Rank)[0];
        const std::int32_t Top    = At.Y + 1, Bottom = At.Y + Size.Rows - 2;
        Put(Out, Middle - 1, Top, Glyphs::CornerTL, Ink, Shown.CardFace);
        Put(Out, Middle, Top, Glyphs::RuleAcross, Ink, Shown.CardFace);
        Put(Out, Middle + 1, Top, Glyphs::CornerTR, Ink, Shown.CardFace);
        for (std::int32_t Row = Top + 1; Row < Bottom; Row++) {
            const bool Edge = Row == Top + 1 || Row == Bottom - 1;
            Put(Out, Middle - 1, Row, Glyphs::RuleDown, Ink, Shown.CardFace);
            Put(Out, Middle + 1, Row, Glyphs::RuleDown, Ink, Shown.CardFace);
            Put(Out, Middle, Row, Edge ? Letter : Mark, Ink, Shown.CardFace);
        }
        Put(Out, Middle - 1, Bottom, Glyphs::CornerBL, Ink, Shown.CardFace);
        Put(Out, Middle, Bottom, Glyphs::RuleAcross, Ink, Shown.CardFace);
        Put(Out, Middle + 1, Bottom, Glyphs::CornerBR, Ink, Shown.CardFace);
        return;
    }
    for (const Pip &Spot : PipsOf(One.Rank))
        Put(Out, Middle + Spot.Col, At.Y + 1 + static_cast<std::int32_t>(Spot.Row * static_cast<float>(Size.Rows - 3) + 0.5f), Mark, Ink,
            Shown.CardFace);
}

}  // namespace

CardSize CardSizeFor(std::int32_t GlassRows)
{
    if (GlassRows < 34) return CardSize{9, 7};
    if (GlassRows < 42) return CardSize{13, 10};
    if (GlassRows < 60) return CardSize{19, 15};
    return CardSize{23, 18};
}

void PaintCard(tui::Frame &Out, const Look &Shown, const Card *Face, painter::core::Point At, CardSize Size)
{
    Canvas Pixels(Out, Hard(Out));
    Body(Pixels, Shown, Face, !Size.Glyphs(), static_cast<float>(At.X), static_cast<float>(At.Y * 2), static_cast<float>(Size.Wide()),
         static_cast<float>(Size.High()));
    if (!Face) return;
    Indices(Out, Shown, *Face, At, Size);
    if (Size.Glyphs()) GlyphPips(Out, Shown, *Face, At, Size);
}

void PaintMoving(tui::Frame &Out, const Look &Shown, const Card *Face, float Across, float Down, CardSize Size, float Squeeze)
{
    Canvas      Pixels(Out, Hard(Out));
    const float Wide = std::max(static_cast<float>(Size.Wide()) * Squeeze, 0.5f);
    Body(Pixels, Shown, Face, true, Across + (static_cast<float>(Size.Wide()) - Wide) / 2.0f, Down, Wide, static_cast<float>(Size.High()));
}

}  // namespace casino
