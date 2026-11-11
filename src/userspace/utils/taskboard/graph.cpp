/* graph.cpp — the graphs and meters. */

#include "graph.h"

#include <algorithm>
#include <cmath>

#include "painter/tui/glyph.h"

namespace taskboard {

namespace {

namespace Glyphs = painter::tui::Glyphs;
using painter::core::Point;

// The faint dotted lines a graph is read against, and what is not lit of a meter.
constexpr std::uint8_t GridWeight = 22;
constexpr std::uint8_t DotWeight  = 70;
// How much brighter the top pixel of a column is: its edge reads as a line.
constexpr std::uint8_t EdgeWeight = 70;

}  // namespace

void DrawGraph(painter::tui::Frame &Out, painter::core::Rect Area, const std::vector<std::optional<float>> &Columns,
               const Look &Shown, Color Paper)
{
    if (Area.Empty()) return;
    const std::int32_t Pixels = Area.Height * 2;
    const Color        Grid   = Color::Mix(Paper, painter::core::Colors::White, GridWeight);
    const bool         Lined  = Area.Height >= 4;
    const auto         OnGrid = [&](std::int32_t X, std::int32_t Level) {
        return Lined && X % 2 == 0 && (Level == Pixels / 4 || Level == Pixels / 2 || Level == Pixels * 3 / 4);
    };
    // A column's colors, floor to top: the person's cool to hot by height.
    std::vector<Color> Ramp(static_cast<std::size_t>(Pixels));
    for (std::int32_t Level = 0; Level < Pixels; Level++)
        Ramp[static_cast<std::size_t>(Level)] = LoadColor(Shown, (Level + 0.5) / Pixels);

    for (std::int32_t X = 0; X < Area.Width; X++) {
        const std::size_t           Index = Columns.size() >= static_cast<std::size_t>(Area.Width)
                                                ? Columns.size() - static_cast<std::size_t>(Area.Width - X)
                                                : static_cast<std::size_t>(-1);
        const std::optional<float> Load  = Index < Columns.size() ? Columns[Index] : std::nullopt;
        const double               Lit   = Load ? std::clamp(static_cast<double>(*Load), 0.0, 1.0) * Pixels : 0;
        const std::int32_t         Whole = static_cast<std::int32_t>(std::floor(Lit));
        const auto                 Part  = static_cast<std::uint8_t>((Lit - Whole) * 255.0);
        for (std::int32_t Level = 0; Level < Pixels; Level++) {
            const Color Behind = OnGrid(X, Level) ? Grid : Paper;
            Color       Ink    = Behind;
            if (Level < Whole) {
                Ink = Ramp[static_cast<std::size_t>(Level)];
                if (Level == Whole - 1 && Part == 0) Ink = Color::Mix(Ink, painter::core::Colors::White, EdgeWeight);
            } else if (Level == Whole && Part > 0) {
                Ink = Color::Mix(Behind, Color::Mix(Ramp[static_cast<std::size_t>(Level)],
                                                    painter::core::Colors::White, EdgeWeight), Part);
            }
            Out.PutPixel(Point{Area.X + X, Area.Y * 2 + Pixels - 1 - Level}, Ink);
        }
    }
}

void DrawMeter(painter::tui::Frame &Out, Point At, std::int32_t Width, double Share, const Look &Shown, Color Paper)
{
    if (Width <= 0) return;
    const std::int32_t Halves = static_cast<std::int32_t>(std::lround(std::clamp(Share, 0.0, 1.0) * Width * 2));
    const Color        Dot    = Color::Mix(Paper, painter::core::Colors::White, DotWeight);
    for (std::int32_t Cell = 0; Cell < Width; Cell++) {
        const Color Ink = LoadColor(Shown, (Cell + 0.5) / Width);
        if (Cell * 2 + 2 <= Halves) Out.Put(Point{At.X + Cell, At.Y}, Glyphs::Full, {Ink, Paper});
        else if (Cell * 2 + 1 == Halves) Out.Put(Point{At.X + Cell, At.Y}, Glyphs::LeftHalf, {Ink, Paper});
        else Out.Put(Point{At.X + Cell, At.Y}, Glyphs::Dot, {Dot, Paper});
    }
}

}  // namespace taskboard
