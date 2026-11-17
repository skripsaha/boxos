/* slots_paint.cpp — the slot machine as it is drawn: the cabinet and its
 * bulbs, the three reels with motion blur, the lines that won, the lever, and
 * the panel beside it. */

#include <algorithm>
#include <cmath>
#include <format>

#include "chips.h"
#include "common.h"
#include "ornament.h"
#include "painter/tui/glyph.h"
#include "painter/tui/text.h"
#include "slots.h"
#include "trig.h"

namespace casino {

namespace {

namespace tui = painter::tui;

const Color Cream  = Color::Rgb(243, 233, 207);
const Color Pane   = Color::Rgb(16, 16, 18);
const Color Metal  = Color::Rgb(120, 120, 128);

constexpr std::int32_t ReelPad  = 6;
constexpr std::int32_t LeverWide = 8;
constexpr std::uint64_t LeverUs = 400000;

Color SymbolInk(Symbol Which)
{
    switch (Which) {
    case Symbol::Seven:  return Color::Rgb(0xE8, 0x38, 0x40);
    case Symbol::Bar:    return Color::Rgb(0xF0, 0xF0, 0xF0);
    case Symbol::Bell:   return Color::Rgb(0xF2, 0xC1, 0x4E);
    case Symbol::Star:   return Color::Rgb(0xFF, 0xE0, 0x70);
    case Symbol::Cherry: return Color::Rgb(0xE8, 0x38, 0x40);
    case Symbol::Lemon:  return Color::Rgb(0xF5, 0xE0, 0x2C);
    }
    return Color::Rgb(255, 255, 255);
}

}  // namespace

Slots::Layout Slots::Measure(const tui::Frame &Out)
{
    Layout Where;
    Where.Cols = Out.Size().Width;
    Where.Rows = Out.Size().Height;
    const std::int32_t Body = Where.Rows - 3;
    Where.Letters           = 1;
    std::int32_t Edge       = (2 * Body - (7 * Where.Letters + 5) - 8) / 3;
    if (Edge >= 20) {
        Where.Letters = 2;
        Edge          = (2 * Body - (7 * Where.Letters + 5) - 8) / 3;
    }
    Where.Edge        = std::clamp(std::min(Edge, (Where.Cols - 52) / 3), 6, 40);
    Where.MarqueeHigh = 7 * Where.Letters + 5;
    Where.CabHigh     = Where.MarqueeHigh + 3 * Where.Edge + 4 + 4;
    Where.CabWide     = 3 * (Where.Edge + 2) + 2 * 2 + 2 * ReelPad;
    Where.PanelWide   = std::clamp(Where.Cols - Where.CabWide - LeverWide - 8, 18, 34);
    Where.CabLeft     = std::max(1, (Where.Cols - (Where.CabWide + LeverWide + 4 + Where.PanelWide)) / 2);
    Where.CabTop      = 2 + std::max(0, (2 * Body - Where.CabHigh) / 2);
    Where.WindowTop   = Where.CabTop + Where.MarqueeHigh;
    Where.PanelLeft   = Where.CabLeft + Where.CabWide + LeverWide + 3;
    if (!Art || Art->Edge() != Where.Edge) Art.emplace(Where.Edge);
    return Where;
}

void Slots::PaintLights(Canvas &Draw, const Layout &Where, std::uint64_t NowUs)
{
    const bool          Fast = Celebrating(NowUs);
    const std::uint64_t Step = NowUs / (Fast ? 70000 : 400000);
    const std::int32_t  Gap  = 5;
    const float Left = Where.CabLeft + 1.5f, Top = Where.CabTop + 1.5f;
    const float Right = Where.CabLeft + Where.CabWide - 1.5f, Bottom = Where.CabTop + Where.CabHigh - 1.5f;
    const std::int32_t Across = static_cast<std::int32_t>(Right - Left), Down = static_cast<std::int32_t>(Bottom - Top);
    const std::int32_t Around = 2 * (Across + Down);
    for (std::int32_t At = 0; At < Around; At += Gap) {
        float PointAcross, PointDown;
        if (At < Across) {
            PointAcross = Left + static_cast<float>(At);
            PointDown   = Top;
        } else if (At < Across + Down) {
            PointAcross = Right;
            PointDown   = Top + static_cast<float>(At - Across);
        } else if (At < 2 * Across + Down) {
            PointAcross = Right - static_cast<float>(At - Across - Down);
            PointDown   = Bottom;
        } else {
            PointAcross = Left;
            PointDown   = Bottom - static_cast<float>(At - 2 * Across - Down);
        }
        Bulb(Draw, PointAcross, PointDown, Ctx.Shown.Lights, (static_cast<std::uint64_t>(At / Gap) + Step) % 3 == 0);
    }
}

void Slots::PaintCabinet(Canvas &Draw, const Layout &Where, std::uint64_t NowUs)
{
    const Color Body = Ctx.Shown.Cabinet;
    Draw.Fill(Where.CabLeft, Where.CabTop, Where.CabWide, Where.CabHigh, Shade(Body, 0.55f));
    Draw.Fill(Where.CabLeft + 1, Where.CabTop + 1, Where.CabWide - 2, Where.CabHigh - 2, Body);
    Draw.Fill(Where.CabLeft + 1, Where.CabTop + 1, 2, Where.CabHigh - 2, Shade(Body, 1.25f));
    Draw.Fill(Where.CabLeft + Where.CabWide - 3, Where.CabTop + 1, 2, Where.CabHigh - 2, Shade(Body, 0.7f));
    Draw.Fill(Where.CabLeft + 3, Where.WindowTop, Where.CabWide - 6, 3 * Where.Edge + 4, Pane);
    Draw.Fill(Where.CabLeft + 3, Where.WindowTop - 1, Where.CabWide - 6, 1, Shade(Ctx.Shown.Gold, 0.8f));
    Draw.Fill(Where.CabLeft + 3, Where.WindowTop + 3 * Where.Edge + 4, Where.CabWide - 6, 1, Shade(Ctx.Shown.Gold, 0.8f));
    const std::int32_t Tray = Where.WindowTop + 3 * Where.Edge + 5;
    Draw.Fill(Where.CabLeft + 6, Tray, Where.CabWide - 12, 2, Shade(Body, 0.3f));
    PaintMarquee(Draw, Where, NowUs);
}

void Slots::PaintMarquee(Canvas &Draw, const Layout &Where, std::uint64_t NowUs)
{
    const bool        Won   = !Running && !Wins.empty();
    const bool        Blink = (NowUs / 130000) % 2 == 0;
    const std::string Words = Celebrating(NowUs) ? "JACKPOT" : Won ? "WINNER" : "LUCKY 7";
    const Color       Ink   = Celebrating(NowUs) && Blink ? painter::core::Colors::White : Ctx.Shown.Gold;
    Draw.Fill(Where.CabLeft + 2, Where.CabTop + 2, Where.CabWide - 4, 7 * Where.Letters + 2, Shade(Ctx.Shown.Cabinet, 0.25f));
    const std::int32_t Wide = Canvas::LettersWide(Words, Where.Letters);
    Draw.Letters(Where.CabLeft + (Where.CabWide - Wide) / 2, Where.CabTop + 3, Words, Where.Letters, Ink);
}

void Slots::PaintReel(Canvas &Draw, const Layout &Where, int Reel, std::int32_t Left, float Time)
{
    const std::int32_t Edge  = Where.Edge;
    const std::int32_t Top   = Where.WindowTop + 2;
    const std::int32_t High  = 3 * Edge;
    const float        Pace2 = Pace(Reel, Time) * static_cast<float>(Ctx.Shown.Speed) / 100.0f;
    const float        Span  = std::min(1.1f, Pace2 * 0.03f);
    const int          Taken = Span > 0.03f ? 6 : 1;
    const float        Spot  = Position(Reel, Time);
    for (std::int32_t Down = 0; Down < High; Down++) {
        const float Near  = std::min(static_cast<float>(Down), static_cast<float>(High - 1 - Down)) / (static_cast<float>(High) * 0.16f);
        const float Round = 0.62f + 0.38f * std::min(Near, 1.0f);
        for (std::int32_t Across = 0; Across < Edge + 2; Across++) {
            std::uint32_t Red = 0, Green = 0, Blue = 0;
            for (int Sample = 0; Sample < Taken; Sample++) {
                const float Off  = Taken == 1 ? 0.0f : (static_cast<float>(Sample) / (Taken - 1) - 0.5f) * Span;
                const float Rise = Spot + Off + 1.5f - (static_cast<float>(Down) + 0.5f) / static_cast<float>(Edge);
                const int   Stop = static_cast<int>(std::floor(Rise + 0.5f));
                const int   Row  = std::clamp(static_cast<int>((static_cast<float>(Stop) + 0.5f - Rise) * static_cast<float>(Edge)), 0, Edge - 1);
                const Rgba  Face = Art->At(OnStrip(Stop), Across - 1, Row);
                // The face laid over the strip's cream by how much of the pixel
                // it covers, and the samples seen together as their light.
                const Color Laid = Color::Over(Cream, Color::Rgb(Face.Red, Face.Green, Face.Blue), Face.Cover);
                Red += BoxColorLight(Laid.Red());
                Green += BoxColorLight(Laid.Green());
                Blue += BoxColorLight(Laid.Blue());
            }
            const auto  Seen = static_cast<std::uint32_t>(Taken);
            const Color Mean = Color::Rgb(static_cast<std::uint8_t>(BoxColorCode((Red + Seen / 2) / Seen)),
                                          static_cast<std::uint8_t>(BoxColorCode((Green + Seen / 2) / Seen)),
                                          static_cast<std::uint8_t>(BoxColorCode((Blue + Seen / 2) / Seen)));
            Draw.Plot(Left + Across, Top + Down, Shade(Mean, Round));
        }
    }
}

void Slots::PaintReels(Canvas &Draw, const Layout &Where, std::uint64_t NowUs)
{
    const float Time = Running ? Seconds(NowUs) : 1e6f;
    for (int Reel = 0; Reel < ReelCount; Reel++)
        PaintReel(Draw, Where, Reel, Where.CabLeft + ReelPad + Reel * (Where.Edge + 4), Time);
    const std::int32_t Middle = Where.WindowTop + 2 + 3 * Where.Edge / 2;
    const std::int32_t Right  = Where.CabLeft + Where.CabWide - 3;
    for (int Row = 0; Row < (Lines > 1 ? 3 : 1); Row++) {
        const std::int32_t Down = Lines > 1 ? Where.WindowTop + 2 + Where.Edge * Row + Where.Edge / 2 : Middle;
        const Color        Mark = Row == 1 || Lines == 1 ? Color::Rgb(0xFF, 0x50, 0x50) : Ctx.Shown.Lights;
        Draw.Fill(Where.CabLeft + 3, Down - 1, 3, 3, Mark);
        Draw.Fill(Right - 3, Down - 1, 3, 3, Mark);
    }
}

void Slots::PaintWins(Canvas &Draw, const Layout &Where, std::uint64_t NowUs)
{
    if (Running || Wins.empty()) return;
    const bool         Pulse = (NowUs / 300000) % 2 == 0;
    const Color        Glow  = Pulse ? painter::core::Colors::White : Ctx.Shown.Lights;
    const std::int32_t Edge  = Where.Edge;
    const std::int32_t Top   = Where.WindowTop + 2;
    for (const Win &One : Wins) {
        const Line &Path = LineAt(One.Line);
        float       LastAcross = 0, LastDown = 0;
        for (int Reel = 0; Reel < One.Reels; Reel++) {
            const std::int32_t Left = Where.CabLeft + ReelPad + Reel * (Edge + 4);
            const std::int32_t Down = Top + Path.Row[Reel] * Edge;
            Draw.Fill(Left, Down, Edge + 2, 1, Glow);
            Draw.Fill(Left, Down + Edge - 1, Edge + 2, 1, Glow);
            Draw.Fill(Left, Down, 1, Edge, Glow);
            Draw.Fill(Left + Edge + 1, Down, 1, Edge, Glow);
            const float Across = static_cast<float>(Left) + static_cast<float>(Edge + 2) / 2.0f;
            const float Mid    = static_cast<float>(Down) + static_cast<float>(Edge) / 2.0f;
            if (Reel > 0) Draw.Line(LastAcross, LastDown, Across, Mid, 1.0f, Glow);
            LastAcross = Across;
            LastDown   = Mid;
        }
    }
}

void Slots::PaintLever(Canvas &Draw, const Layout &Where, std::uint64_t NowUs)
{
    const float Across = static_cast<float>(Where.CabLeft + Where.CabWide) + 3.5f;
    const float Base   = static_cast<float>(Where.WindowTop) + static_cast<float>(3 * Where.Edge) / 2.0f + 4.0f;
    const float Travel = static_cast<float>(Where.Edge) * 1.3f;
    float       Pulled = 0.0f;
    if (Running && NowUs - PulledUs < LeverUs) Pulled = Sine(Pi * static_cast<float>(NowUs - PulledUs) / static_cast<float>(LeverUs));
    Draw.Fill(Where.CabLeft + Where.CabWide, static_cast<std::int32_t>(Base) - 2, 3, 5, Metal);
    const float Ball = Base - Travel * 0.9f + Travel * 1.4f * Pulled;
    Draw.Line(Across, Base, Across, Ball, 1.6f, Metal);
    Draw.Disc(Across, Ball, 2.6f, Shade(Color::Rgb(0xE0, 0x28, 0x30), 0.7f));
    Draw.Disc(Across - 0.6f, Ball - 0.6f, 1.2f, Color::Rgb(0xFF, 0x90, 0x90));
}

void Slots::PaintPanel(tui::Frame &Out, const Layout &Where)
{
    const Look        &Shown = Ctx.Shown;
    const Color        Back  = Color::Rgb(0x14, 0x10, 0x10);
    const Standing    &Now   = Ctx.Chips.Now();
    const tui::TextStyle Said{Shown.Text, Back};
    const tui::TextStyle Quiet{Shown.Quiet, Back};
    const painter::core::Rect Area{Where.PanelLeft - 1, 2, Where.PanelWide + 2, 17};
    Panel(Out, Area, Shade(Shown.Gold, 0.7f), Back, "SLOT MACHINE", Shown.Gold);
    std::int32_t Row = Area.Y + 1;
    const auto Line = [&](std::string_view Label, const std::string &Value, tui::TextStyle Style) {
        Out.Write({Where.PanelLeft, Row}, Label, Quiet);
        Out.Write({Where.PanelLeft + Where.PanelWide - static_cast<std::int32_t>(Value.size()), Row}, Value, Style);
        Row++;
    };
    Line("chips", Counted(Now.Chips), Said);
    Line("bet on a line", Counted(Bet()), Said);
    Line("lines", std::to_string(Lines), Said);
    Line("total bet", Counted(Stake()), Said);
    BoxRule(Out, Area, Row++, Shade(Shown.Gold, 0.7f), Back);
    Line("last win", Running ? std::string("...") : Counted(Paid), Paid > 0 && !Running ? tui::TextStyle{Shown.Won, Back} : Said);
    const std::int64_t Net = Ctx.Chips.Net();
    Line("this visit", (Net > 0 ? "+" : "") + Counted(Net), Net >= 0 ? tui::TextStyle{Shown.Won, Back} : tui::TextStyle{Shown.Lost, Back});
    BoxRule(Out, Area, Row++, Shade(Shown.Gold, 0.7f), Back);
    Line("rounds won", std::format("{} of {}", Now.Wins, Now.Rounds), Said);
    Line("biggest win", Counted(Now.Biggest), Said);
    Line("jackpots", std::to_string(Now.Jackpots), Said);
    const std::int32_t Floor   = Where.Rows - 3;
    const std::int32_t Tallest = 2 * (Floor - (Area.Y + Area.Height)) - 1;
    if (Tallest >= 4) Stacks(Out, Shown, Now.Chips, Where.PanelLeft, Floor, Tallest, Where.PanelWide);
}

void Slots::Paint(tui::Frame &Out, std::uint64_t NowUs)
{
    const Layout Where = Measure(Out);
    GlassWide          = Where.Cols;
    GlassHigh          = Out.PixelHeight();
    Out.Fill(painter::core::Rect{0, 1, Where.Cols, Where.Rows - 3}, ' ', tui::TextStyle{Ctx.Shown.Page, Ctx.Shown.Page});
    Canvas Draw(Out);
    PaintCabinet(Draw, Where, NowUs);
    PaintReels(Draw, Where, NowUs);
    PaintWins(Draw, Where, NowUs);
    PaintLights(Draw, Where, NowUs);
    PaintLever(Draw, Where, NowUs);
    Coins.Paint(Draw, Ctx.Shown.Gold);
    PaintPanel(Out, Where);
    const std::vector<Hint> Hints = {{"space", "pull"}, {"+/-", std::format("bet {}", Counted(Bet()))}, {"l", std::format("lines {}", Lines)},
                                     {Spelled(Ctx.Shown.Keys[KeyPay]), "pay table"}};
    HintBar(Out, Ctx.Shown, Hints);
}

void Slots::PaintPay(tui::Frame &Out)
{
    const Look        &Shown = Ctx.Shown;
    const Color        Back  = Color::Rgb(0x10, 0x10, 0x10);
    const tui::TextStyle Said{Shown.Text, Back};
    const tui::TextStyle Quiet{Shown.Quiet, Back};
    const std::span<const Pay> Table = PayTable();
    const std::int32_t Wide = 66, High = static_cast<std::int32_t>(Table.size()) + 8;
    const painter::core::Rect Area{(Out.Size().Width - Wide) / 2, std::max(1, (Out.Size().Height - High) / 2), Wide, High};
    Box(Out, Area, Shown.Gold, Back, "Pay table: what a line pays for each chip bet on it", Shown.Gold);
    const Color        Grid = Shade(Shown.Gold, 0.55f);
    std::int32_t       Row  = Area.Y + 2;
    const std::int32_t Cut  = Area.X + 33;
    for (const Pay &One : Table) {
        for (int Reel = 0; Reel < ReelCount; Reel++) {
            const std::string Name = One.Any[Reel] ? "any" : std::string(NameOf(One.Needs[Reel]));
            std::string       Upper(Name);
            std::transform(Upper.begin(), Upper.end(), Upper.begin(), [](char Letter) { return static_cast<char>(Letter - 'a' + 'A'); });
            Out.Write({Area.X + 3 + Reel * 9, Row}, Upper, One.Any[Reel] ? Quiet : tui::TextStyle{SymbolInk(One.Needs[Reel]), Back});
        }
        Out.Put({Cut, Row}, tui::Glyphs::RuleDown, tui::TextStyle{Grid, Back});
        Out.Write({Cut + 2, Row}, std::format("{:>4} x", One.Multiple), tui::TextStyle{Shown.Gold, Back});
        if (&One == &Table[0]) Out.Write({Cut + 11, Row}, "THE JACKPOT", tui::TextStyle{Shown.Gold, Back});
        Row++;
    }
    BoxRule(Out, Area, Row++, Grid, Back);
    int Counts[SymbolKinds] = {};
    for (int Stop = 0; Stop < StripLength; Stop++) Counts[static_cast<int>(OnStrip(Stop))]++;
    Out.Write({Area.X + 3, Row++}, std::format("Each reel is {} stops:", StripLength), Quiet);
    Out.Write({Area.X + 3, Row++},
              std::format("{} sevens, {} bars, {} bells, {} stars, {} cherries, {} lemons.", Counts[0], Counts[1], Counts[2], Counts[3],
                          Counts[4], Counts[5]),
              Quiet);
    const int Combinations = StripLength * StripLength * StripLength;
    Out.Write({Area.X + 3, Row++},
              std::format("A line pays 1 time in {:.1f}; three sevens, 1 in {}.", static_cast<double>(Combinations) / PayingCombinations(),
                          Combinations / JackpotCombinations()),
              Quiet);
    Out.Write({Area.X + 3, Row}, std::format("Over time the machine gives back {:.2f}% of what is bet.", ReturnToPlayer() * 100.0), Said);
}

}  // namespace casino
