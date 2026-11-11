/* roulette_paint.cpp — the roulette table as it is drawn: the wheel in
 * pixels, seen from above, the betting board in cells, the chips, the
 * results. */

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include "common.h"
#include "painter/tui/glyph.h"
#include "painter/tui/text.h"
#include "roulette.h"
#include "trig.h"
#include "wheel_plate.h"

namespace casino {

namespace {

namespace tui    = painter::tui;
namespace Glyphs = painter::tui::Glyphs;

constexpr std::uint64_t BlinkUs = 4000000;

const Color Dark  = Color::Rgb(0x14, 0x14, 0x14);
const Color Paper = Color::Rgb(0xF0, 0xF0, 0xF0);
const Color Ivory = Color::Rgb(0xFA, 0xF8, 0xEC);

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

Roulette::Layout Roulette::Measure(const tui::Frame &Out) const
{
    Layout Where;
    Where.Cols = Out.Size().Width;
    Where.Rows = Out.Size().Height;
    const std::int32_t Body = Where.Rows - 3;
    Where.CellWide          = std::clamp(Where.Cols * 55 / 100 / BoardWide, 3, 7);
    Where.CellHigh          = Where.Rows >= 40 && Where.CellWide >= 5 ? 3 : 2;
    Where.BoardLeft         = Where.Cols - BoardWide * Where.CellWide - 1;
    Where.BoardTop          = 2;
    Where.PanelTop          = Where.BoardTop + BoardHigh * Where.CellHigh + 1;
    const std::int32_t Tray = std::clamp(Body / 5, 3, 6);
    const std::int32_t Disc = std::min(Where.BoardLeft - 3, (Body - Tray) * 2) - 8;
    Where.Radius            = static_cast<float>(Disc) / 2.0f;
    Where.WheelAcross       = static_cast<float>(Where.BoardLeft - 1) / 2.0f;
    Where.WheelDown         = 2.0f + static_cast<float>(Body - Tray);
    Where.TrayTop           = 1 + Body - Tray;
    return Where;
}

bool Roulette::Lit(std::uint64_t NowUs) const
{
    return (NowUs / 350000) % 2 == 0;
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

void Roulette::PaintCell(tui::Frame &Out, const Layout &Where, const Place &One, std::uint64_t NowUs)
{
    const Spot &On = One.On;
    Color       Back = Shade(Ctx.Shown.Felt, 0.5f);
    Color       Text = Ctx.Shown.Text;
    if (On.What == Kind::Straight) Back = HueColor(Ctx.Shown, HueOf(On.A));
    if (On.What == Kind::Red) Back = Ctx.Shown.Red;
    if (On.What == Kind::Black) Back = Ctx.Shown.Black;
    const bool Winning = Number >= 0 && !Turning && On.What == Kind::Straight && On.A == Number && (!Blinking(NowUs) || Lit(NowUs));
    if (Laid.On(On) > 0) {
        Back = Ctx.Shown.Gold;
        Text = Dark;
    }
    if (Winning || &Aim.Here() == &One) {
        Back = Paper;
        Text = Dark;
    }

    const painter::core::Rect Area{Where.BoardLeft + One.Left * Where.CellWide, Where.BoardTop + One.Top * Where.CellHigh,
                                   (One.Right - One.Left + 1) * Where.CellWide, (One.Bottom - One.Top + 1) * Where.CellHigh};
    const Color Shadow = Shade(Back, 0.5f);
    Out.Fill(Area, ' ', tui::TextStyle{Text, Back});
    for (std::int32_t Row = Area.Y; Row < Area.Y + Area.Height; Row++)
        Out.Put({Area.X + Area.Width - 1, Row}, Glyphs::RuleDown, tui::TextStyle{Shadow, Back});
    for (std::int32_t Col = Area.X; Col < Area.X + Area.Width - 1; Col++)
        Out.Put({Col, Area.Y + Area.Height - 1}, Glyphs::LowerHalf, tui::TextStyle{Shadow, Back});

    const std::int32_t Room = Area.Width - 1;
    const std::string Words = Label(On, Room);
    const std::int32_t Wide = std::min<std::int32_t>(static_cast<std::int32_t>(Words.size()), Room);
    Out.Write({Area.X + (Room - Wide) / 2, Area.Y + (Area.Height - 1) / 2}, Words.substr(0, static_cast<std::size_t>(Wide)),
              tui::TextStyle{Text, Back});
}

void Roulette::PaintSplits(tui::Frame &Out, const Layout &Where)
{
    for (const Placed &One : Laid.All()) {
        if (One.On.What != Kind::Split) continue;
        const Place *First  = PlaceOf(Spot{Kind::Straight, One.On.A, 0});
        const Place *Second = PlaceOf(Spot{Kind::Straight, One.On.B, 0});
        if (!First || !Second) continue;
        painter::core::Point At;
        if (First->Top == Second->Top) {
            const Place *Left = First->Left < Second->Left ? First : Second;
            At = {Where.BoardLeft + (Left->Right + 1) * Where.CellWide - 1,
                  Where.BoardTop + Second->Top * Where.CellHigh + (Where.CellHigh - 1) / 2};
        } else {
            const Place *Upper = First->Top < Second->Top ? First : Second;
            At = {Where.BoardLeft + Upper->Left * Where.CellWide + (Where.CellWide - 1) / 2,
                  Where.BoardTop + (Upper->Top + 1) * Where.CellHigh - 1};
        }
        Out.Put(At, Glyphs::Bullet, tui::TextStyle{Ctx.Shown.Lights, Out.Read(At).Background});
    }
}

void Roulette::PaintBoard(tui::Frame &Out, const Layout &Where, std::uint64_t NowUs)
{
    const painter::core::Rect Edge{Where.BoardLeft - 1, Where.BoardTop - 1, BoardWide * Where.CellWide + 2,
                                   BoardHigh * Where.CellHigh + 2};
    Out.Fill(Edge, ' ', tui::TextStyle{Shade(Ctx.Shown.Felt, 0.5f), Shade(Ctx.Shown.Felt, 0.5f)});
    for (int Down = 0; Down < BoardHigh; Down++) {
        for (int Across = 0; Across < BoardWide;) {
            const Place *One = PlaceAt(Across, Down);
            if (!One) {
                Across++;
                continue;
            }
            if (One->Left == Across && One->Top == Down) PaintCell(Out, Where, *One, NowUs);
            Across = One->Right + 1;
        }
    }
    PaintSplits(Out, Where);
}

void Roulette::PaintPanel(tui::Frame &Out, const Layout &Where)
{
    const Look        &Shown = Ctx.Shown;
    const tui::TextStyle Said{Shown.Text, Shade(Shown.Felt, 1.0f)};
    const tui::TextStyle Quiet{Shown.Quiet, Shade(Shown.Felt, 1.0f)};
    Canvas Draw(Out);
    const std::int32_t Row = Where.PanelTop;
    Draw.Chip(static_cast<float>(Where.BoardLeft) + 2.5f, static_cast<float>(Row * 2) + 2.5f, 2.6f, ChipColor(Size));
    Out.Write({Where.BoardLeft + 6, Row}, std::format("chip in hand {}", Counted(ChipNow())), Said);
    Out.Write({Where.BoardLeft + 6, Row + 1},
              std::format("on the table {} ({} bets)", Counted(Laid.Total()), Laid.All().size()), Quiet);
    const std::int64_t Net = Ctx.Chips.Net();
    Out.Write({Where.BoardLeft + 6, Row + 2}, std::format("this visit {}{}", Net > 0 ? "+" : "", Counted(Net)),
              tui::TextStyle{Net >= 0 ? Shown.Won : Shown.Lost, Shown.Felt});

    Out.Write({Where.BoardLeft, Row + 3},
              std::format("won {} of {} rounds, best win {}", Ctx.Chips.Now().Wins, Ctx.Chips.Now().Rounds, Counted(Ctx.Chips.Now().Biggest)),
              Quiet);
    const std::int32_t Free = Where.Rows - 2 - (Row + 5);
    if (Number < 0) {
        Out.Write({Where.BoardLeft, Row + 5}, "No spin yet: lay chips, then space.", Quiet);
        return;
    }
    const int           Scale = Free >= 8 ? 2 : 1;
    const std::string   Digits = std::to_string(Number);
    const std::int32_t  Wide  = Draw.LettersWide(Digits, Scale) + 6;
    const std::int32_t  High  = 7 * Scale + 2;
    const std::int32_t  Left  = Where.BoardLeft;
    const std::int32_t  Top   = (Row + 5) * 2;
    Draw.Fill(Left, Top, Wide, High, HueColor(Shown, HueOf(Number)));
    Draw.Fill(Left, Top, Wide, 1, Shown.Gold);
    Draw.Fill(Left, Top + High - 1, Wide, 1, Shown.Gold);
    Draw.Letters(Left + 3, Top + 1, Digits, Scale, painter::core::Colors::White);

    const std::int32_t Beside = Left + Wide + 2;
    const bool Zero = Number == 0;
    Out.Write({Beside, Row + 5}, std::format("{} comes up", Zero ? "zero" : std::to_string(Number)), Said);
    Out.Write({Beside, Row + 6}, std::string(HueName(HueOf(Number))), tui::TextStyle{Said.Foreground, Said.Background});
    if (!Zero) {
        Out.Write({Beside, Row + 7}, std::format("{}, {}", Number % 2 ? "odd" : "even", Number > 18 ? "high" : "low"), Quiet);
        Out.Write({Beside, Row + 8}, std::format("{} dozen, {} column", Ordinal((Number - 1) / 12), Ordinal((Number + 2) % 3)), Quiet);
    }
}

void Roulette::PaintTray(tui::Frame &Out, const Layout &Where)
{
    const std::int32_t Room = Where.BoardLeft - 2;
    const tui::TextStyle Quiet{Ctx.Shown.Quiet, Ctx.Shown.Felt};
    Out.Write({1, Where.TrayTop}, "last", Quiet);
    const std::int32_t Fit = std::max(0, (Room - 6) / 3);
    const std::int32_t Skip = std::max<std::int32_t>(0, static_cast<std::int32_t>(Results.size()) - Fit);
    for (std::int32_t At = Skip; At < static_cast<std::int32_t>(Results.size()); At++) {
        const int Shown = Results[static_cast<std::size_t>(At)];
        Out.Write({6 + (At - Skip) * 3, Where.TrayTop}, std::format("{:^3}", Shown),
                  tui::TextStyle{painter::core::Colors::White, HueColor(Ctx.Shown, HueOf(Shown))});
    }
    const std::int32_t Tallest = 2 * (Where.Rows - 2 - (Where.TrayTop + 1)) - 1;
    if (Tallest >= 4) {
        Canvas Draw(Out);
        Stacks(Draw, Ctx.Shown, Ctx.Chips.Now().Chips, 2, 2 * (Where.Rows - 2), Tallest);
    }
}

void Roulette::PaintHints(tui::Frame &Out, const Layout &Where)
{
    const std::string Hints = std::format("arrows move  enter chip  space spin  +/- size  s split  r repeat  x clear  tab slots  {} keys",
                                          Spelled(Ctx.Shown.Keys[KeyHelp]));
    Out.Write({1, Where.Rows - 1}, tui::Shortened(Hints, static_cast<std::size_t>(Where.Cols - 2)),
              tui::TextStyle{Ctx.Shown.Quiet, Ctx.Shown.Page});
}

void Roulette::Paint(tui::Frame &Out, std::uint64_t NowUs)
{
    const Layout Where = Measure(Out);
    Felt(Out, painter::core::Rect{0, 1, Where.Cols, Where.Rows - 3}, Ctx.Shown.Felt);
    Canvas Draw(Out);
    PaintWheel(Draw, Where, NowUs);
    PaintBoard(Out, Where, NowUs);
    PaintPanel(Out, Where);
    PaintTray(Out, Where);
    PaintHints(Out, Where);
}

void Roulette::PaintPay(tui::Frame &Out)
{
    const std::int32_t Wide = 54, High = 11;
    const painter::core::Rect Area{(Out.Size().Width - Wide) / 2, (Out.Size().Height - High) / 2, Wide, High};
    Box(Out, Area, Ctx.Shown.Gold, Color::Rgb(0x10, 0x10, 0x10), "Roulette pays", Ctx.Shown.Gold);
    const tui::TextStyle Said{Ctx.Shown.Text, Color::Rgb(0x10, 0x10, 0x10)};
    const tui::TextStyle Quiet{Ctx.Shown.Quiet, Color::Rgb(0x10, 0x10, 0x10)};
    const char *const Lines[] = {"one number (straight)            35 to 1", "two numbers (split)               17 to 1",
                                 "twelve: a dozen or a column        2 to 1", "red, black, odd, even, low, high   1 to 1"};
    for (int At = 0; At < 4; At++) Out.Write({Area.X + 3, Area.Y + 2 + At}, Lines[At], Said);
    Out.Write({Area.X + 3, Area.Y + 7}, "One zero on the wheel: 37 pockets, 36 paid.", Quiet);
    Out.Write({Area.X + 3, Area.Y + 8}, "Over time the house keeps 1 chip in 37 (2.70%).", Quiet);
}

}  // namespace casino
