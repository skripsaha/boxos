/* common.cpp — what both tables draw alike (common.h). */

#include "common.h"

#include <algorithm>
#include <format>

#include "painter/tui/glyph.h"

namespace casino {

namespace {

namespace tui    = painter::tui;
namespace Glyphs = painter::tui::Glyphs;

constexpr int ChipMultiples[ChipSizes] = {1, 2, 5, 10, 20, 50, 100};

std::uint32_t Scatter(std::int32_t Across, std::int32_t Down)
{
    std::uint32_t Mixed = static_cast<std::uint32_t>(Across) * 73856093u ^ static_cast<std::uint32_t>(Down) * 19349663u;
    Mixed ^= Mixed >> 13;
    Mixed *= 0x5BD1E995u;
    return Mixed ^ (Mixed >> 15);
}

}  // namespace

std::string Counted(std::int64_t Chips)
{
    std::string Digits = std::to_string(Chips < 0 ? -Chips : Chips);
    for (std::ptrdiff_t At = static_cast<std::ptrdiff_t>(Digits.size()) - 3; At > 0; At -= 3) Digits.insert(static_cast<std::size_t>(At), ",");
    return Chips < 0 ? "-" + Digits : Digits;
}

std::string Spelled(const KeyChord &Chord)
{
    char Spelling[KEY_CHORD_SPELLED_MAX];
    KeyChordSpell(&Chord, Spelling, sizeof(Spelling));
    return Spelling;
}

Color HueColor(const Look &Shown, Hue Which)
{
    static const Color Green = Color::Rgb(0x1E, 0x9A, 0x4A);
    return Which == Hue::Red ? Shown.Red : Which == Hue::Black ? Shown.Black : Green;
}

std::string Entry(std::string_view Keys, std::string_view What)
{
    return std::format("{:<18}{}", Keys, What);
}

void Felt(tui::Frame &Out, painter::core::Rect Area, Color Cloth)
{
    const tui::TextStyle Grain{Shade(Cloth, 1.14f), Cloth};
    const tui::TextStyle Plain{Cloth, Cloth};
    for (std::int32_t Row = Area.Y; Row < Area.Y + Area.Height; Row++) {
        for (std::int32_t Col = Area.X; Col < Area.X + Area.Width; Col++) {
            const bool Grained = (Scatter(Col, Row) & 3) == 0;
            Out.Put({Col, Row}, Grained ? Glyphs::LightShade : ' ', Grained ? Grain : Plain);
        }
    }
}

void Box(tui::Frame &Out, painter::core::Rect Area, Color Edge, Color Fill, std::string_view Title, Color TitleInk)
{
    const tui::TextStyle Rule{Edge, Fill};
    Out.Fill(Area, ' ', Rule);
    const std::int32_t Right = Area.X + Area.Width - 1, Bottom = Area.Y + Area.Height - 1;
    for (std::int32_t Col = Area.X + 1; Col < Right; Col++) {
        Out.Put({Col, Area.Y}, Glyphs::DoubleRule, Rule);
        Out.Put({Col, Bottom}, Glyphs::DoubleRule, Rule);
    }
    for (std::int32_t Row = Area.Y + 1; Row < Bottom; Row++) {
        Out.Put({Area.X, Row}, Glyphs::RuleDown, Rule);
        Out.Put({Right, Row}, Glyphs::RuleDown, Rule);
    }
    Out.Put({Area.X, Area.Y}, static_cast<char>(0xC9), Rule);
    Out.Put({Right, Area.Y}, static_cast<char>(0xBB), Rule);
    Out.Put({Area.X, Bottom}, static_cast<char>(0xC8), Rule);
    Out.Put({Right, Bottom}, static_cast<char>(0xBC), Rule);
    if (!Title.empty()) Out.Write({Area.X + 2, Area.Y}, std::string(" ").append(Title).append(" "), tui::TextStyle{TitleInk, Fill});
}

void Bulb(Canvas &Draw, float Across, float Down, Color Lights, bool Lit)
{
    if (Lit) {
        Draw.Disc(Across, Down, 1.7f, Shade(Lights, 0.45f));
        Draw.Disc(Across, Down, 1.0f, Shade(Lights, 1.35f));
    } else {
        Draw.Disc(Across, Down, 1.0f, Shade(Lights, 0.3f));
    }
}

std::int64_t ChipValue(const Look &Shown, int Size)
{
    return Shown.Step * ChipMultiples[std::clamp(Size, 0, ChipSizes - 1)];
}

Color ChipColor(int Size)
{
    static const Color Colors[ChipSizes] = {
        Color::Rgb(0xE6, 0xE6, 0xE6), Color::Rgb(0xD2, 0x3A, 0x3A), Color::Rgb(0xE0, 0xA0, 0x30), Color::Rgb(0x3A, 0x9A, 0xD2),
        Color::Rgb(0xA0, 0x5C, 0xC0), Color::Rgb(0x44, 0x44, 0x44), Color::Rgb(0xF2, 0xC1, 0x4E)};
    return Colors[std::clamp(Size, 0, ChipSizes - 1)];
}

std::int32_t Stacks(Canvas &Draw, const Look &Shown, std::int64_t Chips, std::int32_t Across, std::int32_t BaseDown,
                    std::int32_t Tallest)
{
    constexpr std::int32_t StackWide = 7;
    constexpr std::int32_t ChipHigh  = 2;
    std::int32_t           Left      = Across;
    int                    Largest   = 0;
    while (Largest + 1 < ChipSizes && Chips / ChipValue(Shown, Largest + 1) >= 4) Largest++;
    for (int Size = Largest; Size >= 0; Size--) {
        const std::int64_t Value = ChipValue(Shown, Size);
        const std::int64_t Count = Chips / Value;
        Chips -= Count * Value;
        if (Count == 0) continue;
        const Color  Body  = ChipColor(Size);
        const auto   Height = static_cast<std::int32_t>(std::min<std::int64_t>(Count, Tallest / ChipHigh));
        for (std::int32_t At = 0; At < Height; At++) {
            const std::int32_t Down = BaseDown - (At + 1) * ChipHigh;
            Draw.Fill(Left, Down, StackWide, 1, Shade(Body, 1.25f));
            Draw.Fill(Left, Down + 1, StackWide, 1, Shade(Body, 0.6f));
            Draw.Plot(Left + 2, Down, Shade(Body, 0.55f));
            Draw.Plot(Left + 4, Down, Shade(Body, 0.55f));
        }
        Left += StackWide + 2;
    }
    return Left - Across;
}

}  // namespace casino
