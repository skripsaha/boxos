/* blackjack_table.cpp — the table itself: the felt with its curved edge and
 * wooden rail, the printing on it, the shoe, the discard tray and the
 * betting circle (blackjack.h). */

#include <algorithm>
#include <cmath>
#include <format>

#include "blackjack.h"
#include "chips.h"
#include "common.h"
#include "ornament.h"
#include "painter/tui/glyph.h"

namespace casino {

namespace {

namespace tui    = painter::tui;
namespace Glyphs = painter::tui::Glyphs;

constexpr char DropDown  = static_cast<char>(0xBF);
constexpr char RiseUp    = static_cast<char>(0xD9);
constexpr char LandRight = static_cast<char>(0xC0);
constexpr char LeapRight = static_cast<char>(0xDA);

// How far a point is outside the table's edge, in pixels, negative inside:
// straight sides above the middle of the arc, a half ellipse below it.
float Outside(float Across, float Down, float Reach, float Depth)
{
    if (Down <= 0.0f) return std::fabs(Across) - Reach;
    const float Far = std::sqrt((Across / Reach) * (Across / Reach) + (Down / Depth) * (Down / Depth));
    const float Slope = std::sqrt(Across * Across / (Reach * Reach * Reach * Reach) + Down * Down / (Depth * Depth * Depth * Depth));
    return Far < 0.001f ? -Depth : (Far - 1.0f) * Far / Slope;
}

// How many rows a character stands above the baseline of a line that curves up
// toward both ends.
std::int32_t Rise(std::int32_t At, std::int32_t Count, std::int32_t Lift)
{
    const float Middle = static_cast<float>(Count - 1) / 2.0f;
    if (Middle <= 0.0f) return 0;
    const float Off = (static_cast<float>(At) - Middle) / Middle;
    return static_cast<std::int32_t>(static_cast<float>(Lift) * Off * Off + 0.5f);
}

void ArcText(tui::Frame &Out, std::int32_t Middle, std::int32_t Row, std::string_view Words, Color Ink, std::int32_t Lift)
{
    const auto Count = static_cast<std::int32_t>(Words.size());
    for (std::int32_t At = 0; At < Count; At++)
        if (Words[static_cast<std::size_t>(At)] != ' ')
            Print(Out, {Middle - Count / 2 + At, Row - Rise(At, Count, Lift)}, std::string(1, Words[static_cast<std::size_t>(At)]), Ink);
}

// A thin line that follows the same curve, joined where it steps a row, with
// a gap of Gap cells left in the middle for words.
void ArcRule(tui::Frame &Out, std::int32_t Middle, std::int32_t Row, std::int32_t Half, std::int32_t Gap, std::int32_t Lift, Color Ink)
{
    const std::int32_t Count = 2 * Half + 1;
    const auto         Put   = [&](std::int32_t Col, std::int32_t Down, char Glyph) {
        Out.Put({Col, Down}, Glyph, tui::TextStyle{Ink, Out.Read({Col, Down}).Background});
    };
    for (std::int32_t At = 0; At < Count; At++) {
        if (std::abs(At - Half) <= Gap) continue;
        const std::int32_t Col = Middle - Half + At;
        const std::int32_t Here = Row - Rise(At, Count, Lift);
        const std::int32_t Next = Row - Rise(At + 1, Count, Lift);
        if (Next > Here) {
            Put(Col, Here, DropDown);
            Put(Col, Next, LandRight);
        } else if (Next < Here) {
            Put(Col, Here, RiseUp);
            Put(Col, Next, LeapRight);
        } else {
            Put(Col, Here, Glyphs::RuleAcross);
        }
    }
}

// The same curve as a fine line, to the fraction of a pixel, with a gap in the middle.
void SmoothArc(Canvas &Draw, float Middle, float Base, float Half, float Gap, float Lift, Color Ink)
{
    float FromAcross = Middle - Half, FromDown = Base - Lift;
    for (float Off = -Half + 1.0f; Off <= Half; Off += 1.0f) {
        const float Across = Middle + Off, Down = Base - Lift * Off * Off / (Half * Half);
        if (std::fabs(Off) > Gap && std::fabs(Off - 1.0f) > Gap) Draw.Line(FromAcross, FromDown, Across, Down, 1.0f, Ink);
        FromAcross = Across;
        FromDown   = Down;
    }
}

}  // namespace

void Blackjack::PaintFelt(tui::Frame &Out, const Layout &Where) const
{
    const Look &Shown = Ctx.Shown;
    Cloth(Out, painter::core::Rect{0, 1, Where.Cols, Where.Bottom}, Shown.Felt);
    Canvas             Draw(Out, Hard(Out));
    const float        Middle = static_cast<float>(Where.Cols) / 2.0f;
    const std::int32_t Down   = 2 * (Where.Bottom + 1);
    for (std::int32_t Row = 2; Row < Down; Row++) {
        for (std::int32_t Col = 0; Col < Where.Cols; Col++) {
            const float Across = static_cast<float>(Col) + 0.5f - Middle, Under = static_cast<float>(Row) + 0.5f - Where.ArcMiddle;
            if (Under <= 0.0f && std::fabs(Across) < Where.ArcAcross - Where.RailWide - 1.0f) continue;
            const float Edge = Outside(Across, Under, Where.ArcAcross, Where.ArcDepth);
            if (Edge < -(Where.RailWide + 1.0f)) continue;
            const float Reach = std::clamp(-Edge / Where.RailWide, 0.0f, 1.0f);
            const Color Wood  = Draw.Smooth() ? Shade(Shown.Rail, 1.1f - 0.35f * Reach) : Shown.Rail;
            const float Outer = std::clamp(0.5f - Edge, 0.0f, 1.0f);
            const float Inner = std::clamp(0.5f - (Edge + Where.RailWide), 0.0f, 1.0f);
            if (Outer < 1.0f) Draw.Plot(Col, Row, Shown.Page);
            if (Outer <= 0.0f) continue;
            if (Outer < 1.0f) Draw.Plot(Col, Row, Wood, Outer);
            else if (Inner <= 0.0f) Draw.Plot(Col, Row, Wood);
            else if (Inner < 1.0f) Draw.Plot(Col, Row, Wood, 1.0f - Inner);
        }
    }
}

void Blackjack::PaintPrinting(tui::Frame &Out, const Layout &Where) const
{
    const Look          &Shown  = Ctx.Shown;
    const BlackjackRules &Rule  = Shown.Blackjack;
    const std::int32_t   Middle = Where.Cols / 2;
    const std::int32_t   Lift   = Where.Rows >= 46 ? 3 : 2;
    const Color          Print1 = Shade(Shown.Gold, 0.85f), Print2 = Shade(Shown.Text, 0.7f), Line = Shade(Shown.Gold, 0.9f);
    ArcText(Out, Middle, Where.TextTop, "BLACKJACK PAYS 3 TO 2", Print1, 0);
    ArcText(Out, Middle, Where.TextTop + 1, Rule.HitSoft17 ? "DEALER HITS SOFT 17" : "DEALER STANDS ON ALL 17s", Print2, 0);
    std::int32_t Row = Where.TextTop + 2;
    if (Where.TextRows >= 4) {
        const std::string Detail = std::format("{} {}  SPLIT UP TO {} HANDS", Rule.Decks, Rule.Decks == 1 ? "DECK" : "DECKS", Rule.MaxHands);
        ArcText(Out, Middle, Row, Detail, Print2, 0);
        Row++;
    }
    const std::string_view Words = Rule.Insurance ? "INSURANCE PAYS 2 TO 1" : "NO INSURANCE";
    const std::int32_t     Half  = std::min(Where.Cols * 36 / 100, Where.Cols / 2 - 8 - static_cast<std::int32_t>(Where.RailWide));
    const auto             Count = static_cast<std::int32_t>(Words.size());
    Canvas                 Draw(Out, Hard(Out));
    if (Draw.Smooth()) SmoothArc(Draw, static_cast<float>(Middle), static_cast<float>(Row * 2 + 1), static_cast<float>(Half), static_cast<float>(Count / 2 + 1),
                                 static_cast<float>(2 * Lift), Line);
    else ArcRule(Out, Middle, Row, Half, Count / 2 + 1, Lift, Line);
    ArcText(Out, Middle, Row, Words, Print1, 0);
}

void Blackjack::PaintShoe(tui::Frame &Out, const Layout &Where) const
{
    Canvas             Draw(Out, Hard(Out));
    const Look        &Shown = Ctx.Shown;
    const std::int32_t Wide = Where.ShoeWide, High = Where.ShoeRows * 2, Left = Where.ShoeLeft, Top = Where.DealerTop * 2;
    const std::int32_t Held = Game.Stock().Held() > 0 ? Game.Stock().Held() : Shown.Blackjack.Decks * DeckSize;
    const std::int32_t Rest = Game.Stock().Held() > 0 ? Game.Stock().Left() : Held;
    const Color        Body = Color::Rgb(0x28, 0x28, 0x30);
    Draw.Rounded(static_cast<float>(Left) + 1.0f, static_cast<float>(Top) + 1.0f, static_cast<float>(Wide), static_cast<float>(High), 1.5f,
                 painter::core::Colors::Black, 0.35f);
    Draw.Rounded(static_cast<float>(Left), static_cast<float>(Top), static_cast<float>(Wide), static_cast<float>(High), 1.5f, Body);
    Draw.Fill(Left + 1, Top, Wide - 2, 1, Shade(Body, 1.5f));
    const std::int32_t Stack = std::max(2, (Wide - 5) * Rest / std::max(1, Held));
    for (std::int32_t Col = 0; Col < Stack; Col++)
        Draw.Fill(Left + 3 + Col, Top + 2, 1, High - 5, Shade(Shown.CardFace, Col % 2 == 0 ? 1.0f : 0.84f));
    const std::int32_t Ahead = Game.Stock().CutAt() - (Game.Stock().Held() - Game.Stock().Left());
    if (Game.Stock().Held() > 0 && Ahead > 0 && Rest > 0)
        Draw.Fill(Left + 3 + std::min(Stack - 1, Stack * Ahead / Rest), Top + 1, 1, High - 3, Shown.CardBack);
    Draw.Fill(Left, Top + High - 3, 3, 2, painter::core::Colors::Black);
    const std::string Said = std::format("shoe {}", Rest);
    Print(Out, {Left + Wide / 2 - static_cast<std::int32_t>(Said.size()) / 2, Where.DealerTop + Where.ShoeRows}, Said, Shown.Quiet);
    if (Game.Stock().Held() > 0 && Game.Stock().CutReached())
        Print(Out, {Left + Wide / 2 - 4, Where.DealerTop + Where.ShoeRows + 1}, "cut card", Shown.Lost);
}

void Blackjack::PaintTray(tui::Frame &Out, const Layout &Where) const
{
    Canvas             Draw(Out, Hard(Out));
    const Look        &Shown = Ctx.Shown;
    const std::int32_t Wide = Where.ShoeWide, High = Where.ShoeRows * 2, Left = Where.TrayLeft, Top = Where.DealerTop * 2;
    const std::int32_t Held = Game.Stock().Held() > 0 ? Game.Stock().Held() : Shown.Blackjack.Decks * DeckSize;
    Draw.Rounded(static_cast<float>(Left), static_cast<float>(Top), static_cast<float>(Wide), static_cast<float>(High), 1.5f,
                 Color::Rgb(0x2A, 0x24, 0x20), 0.7f);
    Draw.Rounded(static_cast<float>(Left) + 1.0f, static_cast<float>(Top) + 1.0f, static_cast<float>(Wide) - 2.0f, static_cast<float>(High) - 2.0f,
                 1.0f, Color::Rgb(0x0C, 0x0C, 0x0C), 0.55f);
    const std::int32_t Pile = static_cast<std::int32_t>(Discarded * (Wide - 4) / std::max(1, Held));
    for (std::int32_t Col = 0; Col < Pile; Col++)
        Draw.Fill(Left + 2 + Col, Top + High - 5, 1, 3, Shade(Shown.CardFace, Col % 2 == 0 ? 0.95f : 0.8f));
    const std::string Said = std::format("discard {}", Discarded);
    Print(Out, {Left + Wide / 2 - static_cast<std::int32_t>(Said.size()) / 2, Where.DealerTop + Where.ShoeRows}, Said, Shown.Quiet);
}

// The betting circle with the chips on it: a pile of chips seen from above, or
// where the circle is too small for one, the bet written in it.
void Blackjack::PaintCircle(tui::Frame &Out, const Layout &Where) const
{
    Canvas       Draw(Out, Hard(Out));
    const Look  &Shown = Ctx.Shown;
    const float  Across = static_cast<float>(Where.Cols) / 2.0f;
    const float  Room   = static_cast<float>(2 * (Where.Bottom + 1)) - Where.RailWide - 1.0f - static_cast<float>(2 * Where.CircleTop);
    const float  Down   = static_cast<float>(2 * Where.CircleTop) + Room / 2.0f;
    const float  Reach  = Room / 2.0f;
    const Color  Ring   = Shade(Shown.Gold, 0.8f);
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
