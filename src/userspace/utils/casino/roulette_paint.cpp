/* roulette_paint.cpp — the roulette table as it is drawn: the wheel in
 * pixels, seen from above, the panel and the results in the font, the chips,
 * the hints (the board is in roulette_board.cpp). */

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include "chips.h"
#include "common.h"
#include "ornament.h"
#include "painter/tui/glyph.h"
#include "painter/tui/text.h"
#include "roulette.h"
#include "trig.h"
#include "wheel_plate.h"

namespace casino {

namespace {

namespace tui = painter::tui;

constexpr std::uint64_t BlinkUs = 4000000;

const Color Ivory = Color::Rgb(0xFA, 0xF8, 0xEC);

const char *Ordinal(int Index)
{
    static const char *const Words[] = {"1st", "2nd", "3rd"};
    return Words[Index];
}

}  // namespace

Roulette::Layout Roulette::Measure(const tui::Frame &Out) const
{
    Layout Where;
    Where.Cols  = Out.Size().Width;
    Where.Rows  = Out.Size().Height;
    Where.Inset = Where.Rows >= 40 ? 2 : 1;
    Where.Floor = Where.Rows - 3 - Where.Inset;
    const std::int32_t Top  = 1 + Where.Inset;
    Where.CellWide          = std::clamp((Where.Cols * 55 / 100 - 1) / BoardWide, 3, 7);
    Where.CellHigh          = Where.Rows >= 56 && Where.CellWide >= 6 ? 4 : Where.Rows >= 40 && Where.CellWide >= 5 ? 3 : 2;
    const std::int32_t Wide = BoardWide * Where.CellWide + 1;
    Where.BoardLeft         = Where.Cols - Where.Inset - 1 - Wide;
    Where.BoardTop          = Top;
    Where.PanelTop          = Where.BoardTop + BoardHigh * Where.CellHigh + 2;
    Where.PanelRows         = 8;
    const std::int32_t Tray = std::clamp((Where.Floor - Top) / 5, 4, 6);
    const std::int32_t Rows = Where.Floor - Tray + 1 - Top;
    const float        Room = std::min(static_cast<float>(Where.BoardLeft - Where.Inset - 1) / 2.0f, static_cast<float>(Rows));
    Where.Radius            = Room - 5.1f;
    Where.WheelAcross       = static_cast<float>(Where.Inset + Where.BoardLeft - 1) / 2.0f;
    Where.WheelDown         = static_cast<float>(2 * Top) + Room + 0.5f;
    Where.TrayTop           = Top + Rows;
    return Where;
}

// The winning number blinks for a while after the wheel is still, and then
// stays lit.
bool Roulette::Blinking(std::uint64_t NowUs) const
{
    return Number >= 0 && !Turning && NowUs - EndedUs < BlinkUs;
}

void Roulette::PaintWheel(Canvas &Draw, const Layout &Where, std::uint64_t NowUs)
{
    const float Radius = Where.Radius;
    if (Radius < 4.0f) return;
    const float Across = Where.WheelAcross, Down = Where.WheelDown;
    Draw.Disc(Across, Down, Radius + 1.6f, Shade(Ctx.Shown.Wheel, 0.4f));
    if (Radius >= 9.0f) {
        const int Bulbs = std::max(16, static_cast<int>(Radius * 1.2f));
        const int Step  = static_cast<int>(NowUs / 220000);
        for (int At = 0; At < Bulbs; At++) {
            const float Angle = TwoPi * At / Bulbs;
            Bulb(Draw, Across + Sine(Angle) * (Radius + 3.4f), Down - Cosine(Angle) * (Radius + 3.4f), Ctx.Shown.Lights,
                 (At + Step) % 3 == 0);
        }
    }

    const bool Showing = Number >= 0 && !Turning;
    const bool Pulse   = !Blinking(NowUs) || Lit(NowUs);
    Plate.Paint(Draw, Ctx.Shown, Ctx.Revision, Across, Down, Radius, Last.WheelAngle, Showing ? SlotOf(Number) : -1,
                Pulse ? 1.0f : 0.0f);

    if (Last.BallRadius <= 0.0f) return;
    const float Reach = Last.BallRadius * Radius;
    const float Rise  = Last.Lift * Radius * 0.07f;
    const float Ball  = std::max(1.1f, Radius * 0.05f) * (1.0f + 0.45f * Last.Lift);
    const float BallAcross = Across + Sine(Last.BallAngle) * Reach;
    const float BallDown   = Down - Cosine(Last.BallAngle) * Reach;
    Draw.Disc(BallAcross + 0.7f + Rise, BallDown + 0.9f + Rise, Ball, Shade(Ctx.Shown.Wheel, 0.2f));
    Draw.Disc(BallAcross, BallDown - Rise, Ball, Ivory);
    Draw.Disc(BallAcross - Ball * 0.3f, BallDown - Rise - Ball * 0.3f, Ball * 0.35f, painter::core::Colors::White);
}

// The chip in hand, the sums, and the number that came up.
void Roulette::PaintPanel(tui::Frame &Out, const Layout &Where)
{
    const Look        &Shown = Ctx.Shown;
    const Color        Back  = Color::Rgb(0x12, 0x12, 0x12);
    const std::int32_t Wide  = BoardWide * Where.CellWide + 1;
    const painter::core::Rect Area{Where.BoardLeft, Where.PanelTop, Wide, Where.PanelRows};
    Panel(Out, Area, Shade(Shown.Gold, 0.7f), Back, "Table", Shown.Gold);
    const tui::TextStyle Said{Shown.Text, Back};
    const tui::TextStyle Quiet{Shown.Quiet, Back};
    const std::int32_t   Left = Area.X + 2, Row = Area.Y + 1;
    Canvas               Draw(Out);
    Draw.Chip(static_cast<float>(Left) + 2.5f, static_cast<float>(Row * 2) + 3.0f, 2.6f, ChipColor(Size));
    Out.Write({Left + 6, Row}, "chip in hand", Quiet);
    Out.Write({Left + 22, Row}, Counted(ChipNow()), Said);
    Out.Write({Left + 6, Row + 1}, "on the table", Quiet);
    Out.Write({Left + 22, Row + 1}, std::format("{} ({} bets)", Counted(Laid.Total()), Laid.All().size()), Said);
    BoxRule(Out, Area, Row + 2, Shade(Shown.Gold, 0.7f), Back);
    const std::int64_t Net = Ctx.Chips.Net();
    Out.Write({Left, Row + 3}, "this visit", Quiet);
    Out.Write({Left + 11, Row + 3}, std::format("{}{}", Net > 0 ? "+" : "", Counted(Net)), tui::TextStyle{Net >= 0 ? Shown.Won : Shown.Lost, Back});
    Out.Write({Left + 22, Row + 3}, std::format("won {} of {}  best {}", Ctx.Chips.Now().Wins, Ctx.Chips.Now().Rounds, Counted(Ctx.Chips.Now().Biggest)), Quiet);
    if (Number < 0) {
        Out.Write({Left, Row + 4}, "No spin yet: lay chips, then space.", Quiet);
        return;
    }
    const bool Zero = Number == 0;
    Out.Write({Left, Row + 4}, std::format(" {:^4} ", Number), tui::TextStyle{painter::core::Colors::White, HueColor(Shown, HueOf(Number))});
    Out.Write({Left + 8, Row + 4}, std::format("{} comes up, {}", Zero ? "zero" : std::to_string(Number), HueName(HueOf(Number))), Said);
    if (!Zero)
        Out.Write({Left + 8, Row + 5}, std::format("{}, {}; {} dozen, {} column", Number % 2 ? "odd" : "even", Number > 18 ? "high" : "low",
                                                    Ordinal((Number - 1) / 12), Ordinal((Number + 2) % 3)), Quiet);
}

// The numbers that came up, newest last, and the chips in hand's purse.
void Roulette::PaintTray(tui::Frame &Out, const Layout &Where)
{
    const Look        &Shown = Ctx.Shown;
    const std::int32_t Left  = Where.Inset + 1;
    const std::int32_t Room  = Where.BoardLeft - Where.Inset - 3;
    Out.Write({Left, Where.TrayTop}, "last", tui::TextStyle{Shown.Quiet, Out.Read({Left, Where.TrayTop}).Background});
    const std::int32_t Fit  = std::max(0, (Room - 5) / 4);
    const std::int32_t Skip = std::max<std::int32_t>(0, static_cast<std::int32_t>(Results.size()) - Fit);
    for (std::int32_t At = Skip; At < static_cast<std::int32_t>(Results.size()); At++) {
        const int Seen = Results[static_cast<std::size_t>(At)];
        Out.Write({Left + 5 + (At - Skip) * 4, Where.TrayTop}, std::format("{:^3}", Seen),
                  tui::TextStyle{painter::core::Colors::White, HueColor(Shown, HueOf(Seen))});
    }
    const std::int32_t Tallest = 2 * (Where.Floor - Where.TrayTop - 1);
    if (Tallest >= 4) Stacks(Out, Shown, Ctx.Chips.Now().Chips, Left, Where.Floor, Tallest, Room);
}

void Roulette::PaintHints(tui::Frame &Out) const
{
    const std::vector<Hint> Hints = {{"arrows", "move"}, {"enter", "chip"}, {"space", "spin"}, {"+/-", std::format("chip {}", Counted(ChipNow()))},
                                     {"s", "split"},     {"r", "repeat"},   {"x", "clear"}};
    HintBar(Out, Ctx.Shown, Hints);
}

void Roulette::Paint(tui::Frame &Out, std::uint64_t NowUs)
{
    const Layout Where = Measure(Out);
    Cloth(Out, painter::core::Rect{0, 1, Where.Cols, Where.Rows - 3}, Ctx.Shown.Felt);
    Rail(Out, painter::core::Rect{0, 1, Where.Cols, Where.Rows - 3}, Ctx.Shown.Rail, Shade(Ctx.Shown.Gold, 0.65f), Where.Inset > 1);
    Canvas Draw(Out);
    PaintWheel(Draw, Where, NowUs);
    PaintBoard(Out, Where, NowUs);
    PaintPanel(Out, Where);
    PaintTray(Out, Where);
    Weave(Out, painter::core::Rect{0, 1, Where.Cols, Where.Rows - 3});
    PaintHints(Out);
}

void Roulette::PaintPay(tui::Frame &Out)
{
    const Look        &Shown = Ctx.Shown;
    const Color        Back  = Color::Rgb(0x10, 0x10, 0x10);
    const std::int32_t Wide = 58, High = 12;
    const painter::core::Rect Area{(Out.Size().Width - Wide) / 2, (Out.Size().Height - High) / 2, Wide, High};
    Box(Out, Area, Shown.Gold, Back, "Roulette pays", Shown.Gold);
    const char *const Lines[][2] = {{"one number (straight)", "35 to 1"},
                                    {"two numbers (split)", "17 to 1"},
                                    {"twelve: a dozen or a column", "2 to 1"},
                                    {"red, black, odd, even, low, high", "1 to 1"}};
    for (int At = 0; At < 4; At++) {
        Out.Write({Area.X + 3, Area.Y + 2 + At}, Lines[At][0], tui::TextStyle{Shown.Text, Back});
        Out.Write({Area.X + Wide - 3 - static_cast<std::int32_t>(std::string_view(Lines[At][1]).size()), Area.Y + 2 + At}, Lines[At][1],
                  tui::TextStyle{Shown.Gold, Back});
    }
    BoxRule(Out, Area, Area.Y + 7, Shown.Gold, Back);
    Out.Write({Area.X + 3, Area.Y + 8}, "One zero on the wheel: 37 pockets, 36 paid.", tui::TextStyle{Shown.Quiet, Back});
    Out.Write({Area.X + 3, Area.Y + 9}, "Over time the house keeps 1 chip in 37 (2.70%).", tui::TextStyle{Shown.Quiet, Back});
}

}  // namespace casino
