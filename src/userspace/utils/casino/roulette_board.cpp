/* roulette_board.cpp — the betting board as a printed layout: thin rules on
 * the cloth, numbers centered in their red, black and green cells, the outside
 * bets in labelled boxes, chips sitting on the places they are laid on
 * (roulette.h). */

#include <algorithm>
#include <format>
#include <string>

#include "board.h"
#include "chips.h"
#include "common.h"
#include "ornament.h"
#include "painter/tui/glyph.h"
#include "roulette.h"

namespace casino {

namespace {

namespace tui = painter::tui;

const Color Dark  = Color::Rgb(0x14, 0x14, 0x14);
const Color Paper = Color::Rgb(0xF0, 0xF0, 0xF0);
const Color Ink   = Color::Rgb(0xEC, 0xE6, 0xD2);

// Which way a rule of the grid runs from a point.
constexpr std::uint8_t ToUp = 1, ToDown = 2, ToRight = 4, ToLeft = 8;

char Joint(std::uint8_t Ways)
{
    switch (Ways) {
    case ToRight | ToLeft:                       return static_cast<char>(0xC4);
    case ToUp | ToDown:                          return static_cast<char>(0xB3);
    case ToDown | ToRight:                       return static_cast<char>(0xDA);
    case ToDown | ToLeft:                        return static_cast<char>(0xBF);
    case ToUp | ToRight:                         return static_cast<char>(0xC0);
    case ToUp | ToLeft:                          return static_cast<char>(0xD9);
    case ToUp | ToDown | ToRight:                return static_cast<char>(0xC3);
    case ToUp | ToDown | ToLeft:                 return static_cast<char>(0xB4);
    case ToDown | ToRight | ToLeft:              return static_cast<char>(0xC2);
    case ToUp | ToRight | ToLeft:                return static_cast<char>(0xC1);
    case ToUp | ToDown | ToRight | ToLeft:       return static_cast<char>(0xC5);
    case ToUp:
    case ToDown:                                 return static_cast<char>(0xB3);
    default:                                     return static_cast<char>(0xC4);
    }
}

const char *Ordinal(int Index)
{
    static const char *const Words[] = {"1st", "2nd", "3rd"};
    return Words[Index];
}

std::string Label(const Spot &On, std::int32_t Room)
{
    switch (On.What) {
    case Kind::Straight: return std::to_string(On.A);
    case Kind::Column:   return Room >= 3 ? "2:1" : std::format("C{}", 3 - On.A);
    case Kind::Dozen:    return std::format("{} 12", Ordinal(On.A));
    case Kind::Low:      return "1-18";
    case Kind::Even:     return "EVEN";
    case Kind::Odd:      return "ODD";
    case Kind::Red:      return "RED";
    case Kind::Black:    return "BLACK";
    case Kind::High:     return "19-36";
    default:             return {};
    }
}

}  // namespace

bool Roulette::Lit(std::uint64_t NowUs) const
{
    return (NowUs / 350000) % 2 == 0;
}

// A place: its fill and label, and the chips on it as a round token with the
// sum written on it.
void Roulette::PaintPlace(tui::Frame &Out, const Layout &Where, const Place &One, std::uint64_t NowUs)
{
    const Spot        &On = One.On;
    const painter::core::Rect In{Where.BoardLeft + One.Left * Where.CellWide + 1, Where.BoardTop + One.Top * Where.CellHigh + 1,
                                 (One.Right - One.Left + 1) * Where.CellWide - 1, (One.Bottom - One.Top + 1) * Where.CellHigh - 1};
    Color Back = Out.Read({In.X, In.Y}).Background;
    Color Text = Ink;
    if (On.What == Kind::Straight) Back = HueColor(Ctx.Shown, HueOf(On.A));
    if (On.What == Kind::Red) Back = Ctx.Shown.Red;
    if (On.What == Kind::Black) Back = Ctx.Shown.Black;
    if (Back != Out.Read({In.X, In.Y}).Background) Text = painter::core::Colors::White;
    const bool Winning = Number >= 0 && !Turning && On.What == Kind::Straight && On.A == Number && (!Blinking(NowUs) || Lit(NowUs));
    if (Winning) {
        Back = Paper;
        Text = Dark;
    }
    Out.Fill(In, ' ', tui::TextStyle{Text, Back});
    const std::int32_t Row   = In.Y + (In.Height - 1) / 2;
    const std::string  Words = Label(On, In.Width);
    const auto         Wide  = std::min<std::int32_t>(static_cast<std::int32_t>(Words.size()), In.Width);
    Out.Write({In.X + (In.Width - Wide) / 2, Row}, Words.substr(0, static_cast<std::size_t>(Wide)), tui::TextStyle{Text, Back});

    const std::int64_t Chips = Laid.On(On);
    if (Chips <= 0) return;
    int Size = 0;
    while (Size + 1 < ChipSizes && Chips >= ChipValue(Ctx.Shown, Size + 1)) Size++;
    const Color        Body  = ChipColor(Size);
    const Color        Inked = Lightness(Body) > 150 ? Dark : painter::core::Colors::White;
    const std::string  Sum   = Brief(Chips);
    const std::int32_t Edge  = In.Width >= 4 ? 1 : 0;
    const std::int32_t Wide2 = std::min<std::int32_t>(In.Width - 2 * Edge, std::max<std::int32_t>(static_cast<std::int32_t>(Sum.size()), 2));
    const std::int32_t Left  = In.X + (In.Width - Wide2) / 2;
    Out.Fill(painter::core::Rect{Left, Row, Wide2, 1}, ' ', tui::TextStyle{Inked, Body});
    Out.Write({Left + (Wide2 - static_cast<std::int32_t>(Sum.size())) / 2, Row}, Sum.substr(0, static_cast<std::size_t>(Wide2)), tui::TextStyle{Inked, Body});
    if (Edge > 0) {
        Out.Put({Left - 1, Row}, tui::Glyphs::RightHalf, tui::TextStyle{Body, Back});
        Out.Put({Left + Wide2, Row}, tui::Glyphs::LeftHalf, tui::TextStyle{Body, Back});
    }
}

// A chip laid on two numbers sits on the rule between them.
void Roulette::PaintSplits(tui::Frame &Out, const Layout &Where)
{
    for (const Placed &One : Laid.All()) {
        if (One.On.What != Kind::Split) continue;
        const Place *First  = PlaceOf(Spot{Kind::Straight, One.On.A, 0});
        const Place *Second = PlaceOf(Spot{Kind::Straight, One.On.B, 0});
        if (!First || !Second) continue;
        painter::core::Point At;
        if (First->Top == Second->Top) {
            const Place *Near = First->Left < Second->Left ? First : Second;
            At = {Where.BoardLeft + (Near->Right + 1) * Where.CellWide, Where.BoardTop + Near->Top * Where.CellHigh + Where.CellHigh / 2};
        } else {
            const Place *Upper = First->Top < Second->Top ? First : Second;
            At = {Where.BoardLeft + Upper->Left * Where.CellWide + Where.CellWide / 2, Where.BoardTop + (Upper->Top + 1) * Where.CellHigh};
        }
        int Size = 0;
        while (Size + 1 < ChipSizes && One.Chips >= ChipValue(Ctx.Shown, Size + 1)) Size++;
        Out.Put(At, static_cast<char>(0x07), tui::TextStyle{ChipColor(Size), Out.Read(At).Background});
    }
}

// The cursor: the place under it ringed in double rules.
void Roulette::PaintCursor(tui::Frame &Out, const Layout &Where)
{
    const Place       &One = Aim.Here();
    const painter::core::Rect Edge{Where.BoardLeft + One.Left * Where.CellWide, Where.BoardTop + One.Top * Where.CellHigh,
                                   (One.Right - One.Left + 1) * Where.CellWide + 1, (One.Bottom - One.Top + 1) * Where.CellHigh + 1};
    const std::int32_t Right = Edge.X + Edge.Width - 1, Bottom = Edge.Y + Edge.Height - 1;
    const auto         Put   = [&](std::int32_t Col, std::int32_t Row, char Glyph) {
        Out.Put({Col, Row}, Glyph, tui::TextStyle{Ctx.Shown.Lights, Out.Read({Col, Row}).Background});
    };
    for (std::int32_t Col = Edge.X + 1; Col < Right; Col++) {
        Put(Col, Edge.Y, static_cast<char>(0xCD));
        Put(Col, Bottom, static_cast<char>(0xCD));
    }
    for (std::int32_t Row = Edge.Y + 1; Row < Bottom; Row++) {
        Put(Edge.X, Row, static_cast<char>(0xBA));
        Put(Right, Row, static_cast<char>(0xBA));
    }
    Put(Edge.X, Edge.Y, static_cast<char>(0xC9));
    Put(Right, Edge.Y, static_cast<char>(0xBB));
    Put(Edge.X, Bottom, static_cast<char>(0xC8));
    Put(Right, Bottom, static_cast<char>(0xBC));
}

void Roulette::PaintBoard(tui::Frame &Out, const Layout &Where, std::uint64_t NowUs)
{
    const std::int32_t Across = BoardWide * Where.CellWide + 1, Down = BoardHigh * Where.CellHigh + 1;
    std::vector<std::uint8_t> Ways(static_cast<std::size_t>(Across * Down), 0);
    const auto Mark = [&](std::int32_t Col, std::int32_t Row, std::uint8_t Toward) { Ways[static_cast<std::size_t>(Row * Across + Col)] |= Toward; };
    for (const Place &One : AllPlaces()) {
        const std::int32_t Left = One.Left * Where.CellWide, Right = (One.Right + 1) * Where.CellWide;
        const std::int32_t Top = One.Top * Where.CellHigh, Bottom = (One.Bottom + 1) * Where.CellHigh;
        for (std::int32_t Col = Left; Col <= Right; Col++) {
            const std::uint8_t Run = static_cast<std::uint8_t>((Col > Left ? ToLeft : 0) | (Col < Right ? ToRight : 0));
            Mark(Col, Top, Run);
            Mark(Col, Bottom, Run);
        }
        for (std::int32_t Row = Top; Row <= Bottom; Row++) {
            const std::uint8_t Run = static_cast<std::uint8_t>((Row > Top ? ToUp : 0) | (Row < Bottom ? ToDown : 0));
            Mark(Left, Row, Run);
            Mark(Right, Row, Run);
        }
    }
    for (const Place &One : AllPlaces()) PaintPlace(Out, Where, One, NowUs);
    const Color Inner = Shade(Ink, 0.78f);
    for (std::int32_t Row = 0; Row < Down; Row++) {
        for (std::int32_t Col = 0; Col < Across; Col++) {
            const std::uint8_t Run = Ways[static_cast<std::size_t>(Row * Across + Col)];
            if (Run == 0) continue;
            const painter::core::Point At{Where.BoardLeft + Col, Where.BoardTop + Row};
            const bool                 Rim = Col == 0 || Row == 0 || Col == Across - 1 || Row == Down - 1;
            Out.Put(At, Joint(Run), tui::TextStyle{Rim ? Ink : Inner, Out.Read(At).Background});
        }
    }
    PaintSplits(Out, Where);
    PaintCursor(Out, Where);
}

}  // namespace casino
