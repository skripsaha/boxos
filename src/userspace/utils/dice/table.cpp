/* table.cpp — the dice's places and their flight. */

#include "table.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include "painter/tui/glyph.h"

#include "shape.h"

namespace dice {

namespace {

namespace tui = painter::tui;
using painter::core::Point;
using painter::core::Rect;
using painter::core::Size;

// A die's side in pixels, the biggest that lets all the dice stand on the table first.
constexpr std::array<int, 7> Sizes = {25, 21, 17, 13, 11, 9, 7};
// A die of a hundred or more faces has three digits to write; the others two.
constexpr int LeastSizeForBigNumbers = 9;
constexpr int Gap                    = 2;
// Cells from one row of tiles to the next, when the table has room for the gap.
constexpr int TileRowStep            = 2;
// The share of the throw in which the dice are let go one after another.
constexpr double Stagger = 0.3;
constexpr double Pi      = 3.14159265358979;
constexpr double Bounces = 3.0;

std::uint64_t Mixed(std::uint64_t Seed)
{
    Seed = (Seed ^ (Seed >> 30)) * 0xBF58476D1CE4E5B9ull;
    Seed = (Seed ^ (Seed >> 27)) * 0x94D049BB133111EBull;
    return Seed ^ (Seed >> 31);
}

Color Toward(Color From, Color To, int Weight)
{
    return Color::Between(From, To, static_cast<std::uint8_t>(Weight));
}

DieStyle StyleOf(int Sides, int Face, bool Counts, bool Settled, const Look &Shown)
{
    DieStyle Style{Shown.Body, Shown.Ink, Toward(Shown.Body, painter::core::Colors::Black, 150),
                   Toward(Shown.Body, painter::core::Colors::Black, 110)};
    const Standing Stands = Settled ? StandingOf(Sides, Face) : Standing::Middle;
    if (Stands != Standing::Middle) {
        const Color Crit = Stands == Standing::High ? Shown.High : Shown.Low;
        Style.Body       = Toward(Style.Body, Crit, 170);
        Style.Edge       = Crit;
    }
    if (Settled && !Counts) {
        for (Color *One : {&Style.Body, &Style.Ink, &Style.Edge, &Style.Line}) *One = Toward(*One, Shown.Table, 140);
    }
    return Style;
}

}  // namespace

bool Table::Settled(std::uint64_t ElapsedMs, const Look &Shown)
{
    return Shown.TumbleMs == 0 || ElapsedMs >= Shown.TumbleMs;
}

void Table::Place(Size Area)
{
    const int Width  = Area.Width;
    const int Height = Area.Height * 2;
    int       Widest = 2;
    for (const Piece &One : Pieces) Widest = std::max(Widest, One.Sides);
    Digits = DigitsOf(Widest);
    Side   = 0;
    for (const int Candidate : Sizes) {
        if (Widest >= 100 && Candidate < LeastSizeForBigNumbers) continue;
        const std::size_t Across = static_cast<std::size_t>(std::max(0, Width / (Candidate + Gap)));
        const std::size_t Down   = static_cast<std::size_t>(std::max(0, Height / (Candidate + Gap)));
        if (Across * Down >= Pieces.size()) {
            Side = Candidate;
            break;
        }
    }
    const int         Spacing  = Side ? Gap : 1;
    const int         ItemWide = Side ? Side : Digits + 2;
    const int         ItemHigh = Side ? Side : 2;
    const int         StepX    = ItemWide + Spacing;
    const std::size_t Across   = static_cast<std::size_t>(std::max(1, (Width + Spacing) / StepX));
    // Tiles are set a row apart when the table has the rows for it.
    const bool        Spread   = !Side && Pieces.size() <= Across * static_cast<std::size_t>(Height / (2 * TileRowStep));
    const int         StepY    = Side ? Side + Gap : Spread ? 2 * TileRowStep : 2;
    const std::size_t Down     = static_cast<std::size_t>(std::max(1, (Height + (StepY - ItemHigh)) / StepY));
    Placed                     = std::min(Pieces.size(), Across * Down);

    const int Rows   = static_cast<int>((Placed + Across - 1) / Across);
    const int Block  = Rows * StepY - (StepY - ItemHigh);
    int       StartY = std::max(0, (Height - Block) / 2);
    if (!Side) StartY &= ~1;
    for (std::size_t Each = 0; Each < Placed; Each++) {
        const int Row     = static_cast<int>(Each / Across);
        const int Column  = static_cast<int>(Each % Across);
        const int InRow   = static_cast<int>(std::min(Across, Placed - Row * Across));
        const int RowWide = InRow * StepX - (StepX - ItemWide);
        Pieces[Each].Rest = {std::max(0, (Width - RowWide) / 2) + Column * StepX, StartY + Row * StepY};
    }
}

void Table::Aim(Source &From)
{
    const int Wide = Side ? Side : Digits + 2;
    const int High = Side ? Side : 2;
    for (std::size_t Each = 0; Each < Placed; Each++) {
        Piece &One = Pieces[Each];
        if (Side) {
            // Not a grid: every die a little off its place, as dice lie.
            const int Right = std::max(0, Cells.Width - Side);
            const int Low   = std::max(0, Cells.Height * 2 - Side);
            One.Rest.X = std::clamp(One.Rest.X + static_cast<int>(From.Below(3)) - 1, 0, Right);
            One.Rest.Y = std::clamp(One.Rest.Y + static_cast<int>(From.Below(3)) - 1, 0, Low);
        }
        const int Top = static_cast<int>(From.Below(static_cast<std::uint64_t>(std::max(1, Cells.Height * 2 - High))));
        One.Start     = {-Wide - 3 - static_cast<int>(Each % 4) * 5, Side ? Top : Top & ~1};
        One.Delay     = Stagger * static_cast<double>(Each) / static_cast<double>(Placed);
        One.Seed      = From.Next();
    }
}

void Table::Throw(const Roll &Thrown, Size Area, Source &From)
{
    Pieces.clear();
    Cells = Area;
    for (const GroupRoll &Group : Thrown.Groups) {
        for (const Die &One : Group.Dice) {
            Piece Next;
            Next.Sides  = Group.Of.Sides;
            Next.Face   = One.Value;
            Next.Counts = One.Counts;
            Pieces.push_back(Next);
        }
    }
    Place(Area);
    Aim(From);
}

Point Table::Where(const Piece &One, double Progress) const
{
    const double Left  = 1.0 - Progress;
    const double Eased = 1.0 - Left * Left * Left;
    const double Lift  = Cells.Height * 2 * 0.35 * std::fabs(std::sin(Bounces * Pi * Progress)) * Left * Left;
    const int    X     = One.Rest.X + static_cast<int>(std::lround((One.Start.X - One.Rest.X) * (1.0 - Eased)));
    int          Y     = One.Rest.Y + static_cast<int>(std::lround((One.Start.Y - One.Rest.Y) * (1.0 - Eased) - Lift));
    if (!Side) Y = (Y >> 1) * 2;
    return {X, Y};
}

void Table::Paint(tui::Frame &Out, Rect Area, std::uint64_t ElapsedMs, const Look &Shown) const
{
    const Canvas On{Out, {Area.X, Area.Y * 2, Area.Width, Area.Height * 2}};
    const Color  Felt = Toward(Shown.Table, painter::core::Colors::Black, 18);
    for (int Y = 0; Y < On.Clip.Height; Y++)
        for (int X = 0; X < On.Clip.Width; X++)
            Out.PutPixel({On.Clip.X + X, On.Clip.Y + Y}, (X + Y) % 2 ? Felt : Shown.Table);

    const double Whole = Shown.TumbleMs ? std::min(1.0, static_cast<double>(ElapsedMs) / Shown.TumbleMs) : 1.0;
    const double Spent = Shown.TumbleMs / 1000.0;
    std::vector<double> Progress(Placed);
    for (std::size_t Each = 0; Each < Placed; Each++)
        Progress[Each] = std::clamp((Whole - Pieces[Each].Delay) / (1.0 - Stagger), 0.0, 1.0);

    const Color Shade = Toward(Shown.Table, painter::core::Colors::Black, 110);
    for (std::size_t Each = 0; Each < Placed && Side; Each++) {
        const Point At = Where(Pieces[Each], Progress[Each]);
        DrawShadow(On, {On.Clip.X + At.X, On.Clip.Y + At.Y}, Side, ShapeOf(Pieces[Each].Sides), Shade);
    }
    for (std::size_t Each = 0; Each < Placed; Each++) {
        const Piece &One     = Pieces[Each];
        const bool   Landed  = Progress[Each] >= 1.0;
        const Point  At      = Where(One, Progress[Each]);
        const Point  Corner  = {On.Clip.X + At.X, On.Clip.Y + At.Y};
        const double Slowing = 1.0 - (1.0 - Progress[Each]) * (1.0 - Progress[Each]);
        const std::uint64_t Flip = static_cast<std::uint64_t>(Shown.FlipsPerSecond * Spent * Slowing);
        const int Face = Landed ? One.Face : static_cast<int>(Mixed(One.Seed + Flip) % static_cast<std::uint64_t>(One.Sides)) + 1;
        const DieStyle Style = StyleOf(One.Sides, Face, One.Counts, Landed, Shown);
        if (Side) DrawDie(On, Corner, Side, One.Sides, Face, Style);
        else      DrawTile(On, Corner, Digits, Face, Style);
    }
    if (Placed < Pieces.size()) {
        const std::string More = std::format(" +{} more ", Pieces.size() - Placed);
        const int         Left = std::max(Area.X, Area.X + Area.Width - static_cast<int>(More.size()));
        Out.Write({Left, Area.Y + Area.Height - 1}, More, {Shown.Quiet, Shown.Table});
    }
}

}  // namespace dice
