/* blackjack_table.cpp — the table itself: the felt with its curved edge and
 * wooden rail, the printing on it, the shoe, the discard tray and the
 * betting circle (blackjack.h). Every edge is settled cell by cell on the
 * glyph of the font that matches it best, so none is a staircase of blocks. */

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include "blackjack.h"
#include "chips.h"
#include "common.h"
#include "ornament.h"
#include "painter/tui/glyph.h"

namespace casino {

namespace {

namespace tui = painter::tui;

enum Zone : int { Beyond = 0, Wood = 1, Felt = 2 };

// How far a point is outside the table's edge, in pixels, negative inside:
// straight sides above the middle of the arc, a half ellipse below it.
float Outside(float Across, float Down, float Reach, float Depth)
{
    if (Down <= 0.0f) return std::fabs(Across) - Reach;
    const float Far   = std::sqrt((Across / Reach) * (Across / Reach) + (Down / Depth) * (Down / Depth));
    const float Slope = std::sqrt(Across * Across / (Reach * Reach * Reach * Reach) + Down * Down / (Depth * Depth * Depth * Depth));
    return Far < 0.001f ? -Depth : (Far - 1.0f) * Far / Slope;
}

void CenterText(tui::Frame &Out, std::int32_t Middle, std::int32_t Row, std::string_view Words, Color Ink)
{
    Print(Out, {Middle - static_cast<std::int32_t>(Words.size()) / 2, Row}, Words, Ink);
}

// A line along y = Base - Lift (x / Half)^2 over Half logical pixels either
// side of Middle, drawn to the fraction of a pixel, with a gap of Gap pixels
// in the middle for words.
void ArcLine(Canvas &Draw, float Middle, float Base, float Half, float Gap, float Lift, Color Ink)
{
    float FromAcross = Middle - Half, FromDown = Base - Lift;
    for (float Off = -Half + 0.5f; Off <= Half; Off += 0.5f) {
        const float Across = Middle + Off, Down = Base - Lift * Off * Off / (Half * Half);
        if (std::fabs(Off) > Gap && std::fabs(Off - 0.5f) > Gap) Draw.Line(FromAcross, FromDown, Across, Down, 1.1f, Ink);
        FromAcross = Across;
        FromDown   = Down;
    }
}

}  // namespace

void Blackjack::PaintFelt(tui::Frame &Out, const Layout &Where) const
{
    const Look &Shown = Ctx.Shown;
    Cloth(Out, painter::core::Rect{0, 1, Where.Cols, Where.Bottom}, Shown.Felt);
    const float Middle = static_cast<float>(Where.Cols) / 2.0f;
    const auto  Edge   = [&](float X, float Y) { return Outside(X - Middle, Y - Where.ArcMiddle, Where.ArcAcross, Where.ArcDepth); };
    for (std::int32_t Row = 1; Row <= Where.Bottom; Row++) {
        for (std::int32_t Col = 0; Col < Where.Cols; Col++) {
            const float Here = Edge(static_cast<float>(Col) + 0.5f, static_cast<float>(2 * Row) + 1.0f);
            if (Here < -(Where.RailWide + 2.0f)) continue;
            std::array<int, 3>           Counts{};
            std::array<std::uint8_t, 128> Zones{};
            for (int Y = 0; Y < 16; Y++)
                for (int X = 0; X < 8; X++) {
                    const float D = Edge(static_cast<float>(Col) + (static_cast<float>(X) + 0.5f) / 8.0f, static_cast<float>(2 * Row) + (static_cast<float>(Y) + 0.5f) / 8.0f);
                    const int   Z = D > 0.0f ? Beyond : D > -Where.RailWide ? Wood : Felt;
                    Zones[static_cast<std::size_t>(Y * 8 + X)] = static_cast<std::uint8_t>(Z);
                    Counts[static_cast<std::size_t>(Z)]++;
                }
            const float Reach = std::clamp(-Here / Where.RailWide, 0.0f, 1.0f);
            const std::array<Color, 3> Inks = {Shown.Page, Shade(Shown.Rail, 1.1f - 0.35f * Reach), Out.Read({Col, Row}).Background};
            int First = 0, Second = -1;
            for (int Z = 1; Z < 3; Z++) if (Counts[static_cast<std::size_t>(Z)] > Counts[static_cast<std::size_t>(First)]) First = Z;
            for (int Z = 0; Z < 3; Z++) if (Z != First && Counts[static_cast<std::size_t>(Z)] > 0 && (Second < 0 || Counts[static_cast<std::size_t>(Z)] > Counts[static_cast<std::size_t>(Second)])) Second = Z;
            if (Second < 0) {
                if (First != Felt) Out.Put({Col, Row}, ' ', tui::TextStyle{Inks[static_cast<std::size_t>(First)], Inks[static_cast<std::size_t>(First)]});
                continue;
            }
            std::array<std::uint8_t, 16> Mask{};
            for (int Y = 0; Y < 16; Y++)
                for (int X = 0; X < 8; X++) {
                    const int Z = Zones[static_cast<std::size_t>(Y * 8 + X)];
                    if (Z == First || (Z != Second && std::abs(Z - First) < std::abs(Z - Second))) Mask[static_cast<std::size_t>(Y)] |= static_cast<std::uint8_t>(0x80 >> X);
                }
            Settle(Out, {Col, Row}, Inks[static_cast<std::size_t>(First)], Inks[static_cast<std::size_t>(Second)], Mask, !Hard(Out));
        }
    }
}

void Blackjack::PaintPrinting(tui::Frame &Out, const Layout &Where) const
{
    const Look          &Shown  = Ctx.Shown;
    const BlackjackRules &Rule  = Shown.Blackjack;
    const std::int32_t   Middle = Where.Cols / 2;
    const Color          Print1 = Shade(Shown.Gold, 0.85f), Print2 = Shade(Shown.Text, 0.7f), Line = Shade(Shown.Gold, 0.9f);
    CenterText(Out, Middle, Where.TextTop, "BLACKJACK PAYS 3 TO 2", Print1);
    CenterText(Out, Middle, Where.TextTop + 1, Rule.HitSoft17 ? "DEALER HITS SOFT 17" : "DEALER STANDS ON ALL 17s", Print2);
    std::int32_t Row = Where.TextTop + 2;
    if (Where.TextRows >= 4) {
        CenterText(Out, Middle, Row, std::format("{} {}  SPLIT UP TO {} HANDS", Rule.Decks, Rule.Decks == 1 ? "DECK" : "DECKS", Rule.MaxHands), Print2);
        Row++;
    }
    const std::string_view Words = Rule.Insurance ? "INSURANCE PAYS 2 TO 1" : "NO INSURANCE";
    const float            Half  = static_cast<float>(std::min(Where.Cols * 36 / 100, Where.Cols / 2 - 8 - static_cast<std::int32_t>(Where.RailWide)));
    Canvas                 Draw(Out, Hard(Out));
    ArcLine(Draw, static_cast<float>(Middle), static_cast<float>(Row * 2 + 1), Half, static_cast<float>(Words.size()) / 2.0f + 1.0f,
            Where.Rows >= 46 ? 7.0f : 4.0f, Line);
    CenterText(Out, Middle, Row, Words, Print1);
}

// The shoe: a framed box with a stack of cards seen from the edge, the cut
// card a red one among them, and the slot the cards leave by.
void Blackjack::PaintShoe(tui::Frame &Out, const Layout &Where) const
{
    const Look        &Shown = Ctx.Shown;
    const Color        Back  = Color::Rgb(0x1A, 0x1A, 0x1A);
    const std::int32_t Held  = Game.Stock().Held() > 0 ? Game.Stock().Held() : Shown.Blackjack.Decks * DeckSize;
    const std::int32_t Rest  = Game.Stock().Held() > 0 ? Game.Stock().Left() : Held;
    const painter::core::Rect Area{Where.ShoeLeft, Where.DealerTop, Where.ShoeWide, Where.ShoeRows};
    Box(Out, Area, Color::Rgb(0x9A, 0x9A, 0x9A), Back, std::format("shoe {}", Rest), Shown.Quiet);
    const std::int32_t Inner = Area.Width - 4, Stack = std::max(1, Inner * Rest / std::max(1, Held));
    const std::int32_t Ahead = Game.Stock().CutAt() - (Game.Stock().Held() - Game.Stock().Left());
    for (std::int32_t Row = Area.Y + 1; Row < Area.Y + Area.Height - 1; Row++) {
        for (std::int32_t At = 0; At < Stack; At++) {
            const bool Cut = Game.Stock().Held() > 0 && Ahead > 0 && At == std::min(Stack - 1, Stack * Ahead / std::max(1, Rest));
            Out.Put({Area.X + 2 + At, Row}, static_cast<char>(0xDE), tui::TextStyle{Cut ? Shown.CardBack : Shown.CardFace, Cut ? Shade(Shown.CardBack, 0.6f) : Shade(Shown.CardFace, 0.7f)});
        }
        Out.Put({Area.X + 1, Row}, static_cast<char>(0xDD), tui::TextStyle{Shade(Back, 0.4f), Back});
    }
    Out.Put({Area.X, Area.Y + Area.Height - 2}, static_cast<char>(0xDB), tui::TextStyle{painter::core::Colors::Black, Back});
    if (Game.Stock().Held() > 0 && Game.Stock().CutReached())
        Print(Out, {Area.X + 2, Area.Y + Area.Height}, "cut card", Shown.Lost);
}

// The discard tray: a framed box and the played cards lying in it, a few
// more of them the more have been played.
void Blackjack::PaintTray(tui::Frame &Out, const Layout &Where) const
{
    const Look        &Shown = Ctx.Shown;
    const Color        Back  = Color::Rgb(0x1A, 0x1A, 0x1A);
    const painter::core::Rect Area{Where.TrayLeft, Where.DealerTop, Where.ShoeWide, Where.ShoeRows};
    Box(Out, Area, Color::Rgb(0x9A, 0x9A, 0x9A), Back, std::format("discard {}", Discarded), Shown.Quiet);
    if (Discarded <= 0) return;
    const std::int32_t Layers = static_cast<std::int32_t>(std::min<std::int64_t>(Area.Height - 2, 1 + Discarded / 12));
    for (std::int32_t Layer = 0; Layer < Layers; Layer++) {
        const std::int32_t Row = Area.Y + Area.Height - 2 - Layer, Slip = (Layer * 5 + 1) % 3;
        const Color        Edge = Shade(Shown.CardFace, Layer % 2 == 0 ? 1.0f : 0.82f);
        for (std::int32_t Col = Area.X + 2 + Slip; Col < Area.X + Area.Width - 3 + Slip; Col++)
            Out.Put({Col, Row}, static_cast<char>(0xDC), tui::TextStyle{Edge, Back});
    }
}

// The betting circle with the chips on it: a fine ring, and inside it a pile
// of chips seen from above, or where the circle is too small for one, the bet
// written in it.
void Blackjack::PaintCircle(tui::Frame &Out, const Layout &Where) const
{
    Canvas       Draw(Out, Hard(Out));
    const Look  &Shown  = Ctx.Shown;
    const float  Across = static_cast<float>(Where.Cols) / 2.0f;
    const float  Room   = static_cast<float>(2 * (Where.Bottom + 1)) - Where.RailWide - 1.0f - static_cast<float>(2 * Where.CircleTop);
    const float  Down   = static_cast<float>(2 * Where.CircleTop) + Room / 2.0f;
    const float  Reach  = Room / 2.0f;
    const Color  Ring   = Shade(Shown.Gold, 0.85f);
    Draw.Ring(Across, Down, Reach, Reach - 1.0f, Ring);
    std::int64_t Bet = 0;
    for (const Hand &One : Game.Hands()) Bet += Playing ? One.Bet : 0;
    if (!Playing) Bet = Afford();
    if (Bet <= 0) return;
    int Size = 0;
    while (Size + 1 < ChipSizes && Bet >= ChipValue(Shown, Size + 1)) Size++;
    const Color Body = ChipColor(Size);
    if (Reach >= 4.0f) {
        const int Pile = std::min(4, 1 + static_cast<int>(Bet / ChipValue(Shown, Size)) / 2);
        for (int At = 0; At < Pile; At++) Draw.Chip(Across, Down - static_cast<float>(At) * 0.9f + 0.9f, Reach - 1.8f, Body);
    }
    const std::string Said = Brief(Bet);
    const painter::core::Point Spot{static_cast<std::int32_t>(Across) - static_cast<std::int32_t>(Said.size()) / 2, static_cast<std::int32_t>(Down) / 2};
    const Color Ink = Lightness(Body) > 150 ? Color::Rgb(0x14, 0x14, 0x14) : painter::core::Colors::White;
    Out.Write(Spot, Said, tui::TextStyle{Ink, Body});
}

}  // namespace casino
