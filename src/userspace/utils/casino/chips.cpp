/* chips.cpp — the play chips (chips.h). */

#include "chips.h"

#include <algorithm>
#include <format>

#include "painter/tui/text.h"

namespace casino {

namespace {

namespace tui = painter::tui;

constexpr int ChipMultiples[ChipSizes] = {1, 2, 5, 10, 20, 50, 100};

// One chip of a stack, its center at Down, Scale times the size of a chip on
// the text console.
void Chip(Canvas &Draw, float Across, float Down, Color Body, bool Top, int Turn, float Scale)
{
    Draw.Ellipse(Across, Down + 0.9f * Scale, 3.5f * Scale, 1.7f * Scale, Shade(Body, 0.5f));
    Draw.Ellipse(Across, Down, 3.3f * Scale, 1.45f * Scale, Body);
    const Color Spot = Shade(Body, 1.9f);
    for (int At = 0; At < 3; At++)
        Draw.Disc(Across + (static_cast<float>((At + Turn) % 3) - 1.0f) * 2.3f * Scale, Down + 1.15f * Scale, 0.45f * Scale, Spot, 0.9f);
    if (Top) Draw.Ellipse(Across, Down, 2.0f * Scale, 0.8f * Scale, Shade(Body, 1.3f));
}

}  // namespace

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

std::string Brief(std::int64_t Chips)
{
    if (Chips < 10000) return std::to_string(Chips);
    if (Chips < 1000000) return std::format("{}k", Chips / 1000);
    if (Chips < 10000000) return std::format("{}.{}M", Chips / 1000000, Chips % 1000000 / 100000);
    return std::format("{}M", Chips / 1000000);
}

std::int32_t Stacks(tui::Frame &Out, const Look &Shown, std::int64_t Chips, std::int32_t Across, std::int32_t FloorRow,
                    std::int32_t Tallest, std::int32_t Room)
{
    Canvas       Draw(Out, Hard(Out));
    const float  Scale     = Out.Size().Height >= 40 ? 1.5f : 1.0f;
    const auto   StackWide = static_cast<std::int32_t>(7.0f * Scale);
    const float  Thick     = 1.25f * Scale;
    const float  Floor   = static_cast<float>(FloorRow * 2);
    std::int32_t Left    = Across;
    int          Largest = 0;
    while (Largest + 1 < ChipSizes && Chips / ChipValue(Shown, Largest + 1) >= 4) Largest++;
    for (int Size = Largest; Size >= 0; Size--) {
        const std::int64_t Value = ChipValue(Shown, Size);
        const std::int64_t Count = Chips / Value;
        Chips -= Count * Value;
        if (Count == 0) continue;
        if (Left + StackWide > Across + Room) break;
        const float Middle = static_cast<float>(Left) + static_cast<float>(StackWide) / 2.0f;
        const auto  Height = static_cast<std::int32_t>(std::min<std::int64_t>(Count, std::max(1.0f, (static_cast<float>(Tallest) - 3.0f * Scale) / Thick)));
        Draw.Ellipse(Middle + 0.9f * Scale, Floor - 0.5f * Scale, 4.2f * Scale, 1.5f * Scale, painter::core::Colors::Black, 0.4f);
        for (std::int32_t At = 0; At < Height; At++) {
            const float Slip = static_cast<float>((At * 37 + Size * 11) % 3 - 1) * 0.35f;
            Chip(Draw, Middle + Slip, Floor - 1.7f * Scale - static_cast<float>(At) * Thick, ChipColor(Size), At == Height - 1, At, Scale);
        }
        const std::string Label = Brief(Value);
        const std::int32_t Room = static_cast<std::int32_t>(Label.size());
        const painter::core::Point At{Left + (StackWide - Room) / 2, FloorRow};
        Out.Write(At, Label, tui::TextStyle{Shown.Text, Out.Read(At).Background});
        Left += std::max(StackWide, Room) + 2;
    }
    return Left - Across;
}

}  // namespace casino
