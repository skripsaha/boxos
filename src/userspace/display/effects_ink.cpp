// effects_ink.cpp — the effects that change a cell's look (box/display.h):
// fade, invert, tint, rainbow, sepia, spotlight, shimmer and snow.

#include "effects_ink.h"

#include <algorithm>
#include <utility>

namespace effects {

namespace {

constexpr char UpperHalf = static_cast<char>(0xDF);
constexpr char LowerHalf = static_cast<char>(0xDC);
constexpr char Full = static_cast<char>(0xDB);
constexpr std::int32_t Sixth = 341;     // a third of a turn is 341 of 1024ths

std::uint32_t Own(std::uint32_t Colour, std::uint32_t Default) noexcept
{
    return (Colour >> 24) ? Default : Colour;
}

// The colour a column of the rainbow shows, at its turn.
std::uint32_t RainbowGlass(std::uint32_t Turn) noexcept
{
    const auto Part = [Turn](std::uint32_t Shift) {
        return static_cast<std::uint32_t>(128 + Sine(Turn + Shift) * 127 / 1024);
    };
    return Pack(Part(0), Part(static_cast<std::uint32_t>(Sixth)), Part(static_cast<std::uint32_t>(2 * Sixth)));
}

void PlanRainbow(Ink &Out, const Moment &Now)
{
    constexpr std::int32_t RootThird = 591;     // sin 60 degrees over 1.5, in 1024ths
    const std::uint32_t Mix = Now.Mix();
    const std::uint32_t Weight = Mix * 3 / 5;
    Out.Rainbow = true;
    Out.Hues.resize(Now.Cols);
    const std::uint32_t Drift = static_cast<std::uint32_t>(Now.PhaseMs * 1024u / 3200u);
    for (std::uint32_t X = 0; X < Now.Cols; X++) {
        const std::uint32_t Turn = Drift + X * 1536u / Now.Cols;
        const std::int32_t Cos = Sine(Turn + 256), Sin = Sine(Turn);
        const std::int32_t Rest = (1024 - Cos) / 3;
        const std::uint32_t Glass = RainbowGlass(Turn);
        Out.Hues[X] = Hue{static_cast<std::int32_t>(Between(1024, static_cast<std::uint32_t>((1024 + 2 * Cos) / 3), Mix)),
                          static_cast<std::int32_t>(Between(0, static_cast<std::uint32_t>(Rest + Sin * RootThird / 1024), Mix)),
                          static_cast<std::int32_t>(Between(0, static_cast<std::uint32_t>(Rest - Sin * RootThird / 1024), Mix)),
                          Between(Whole, Channel(Glass, 16), Weight), Between(Whole, Channel(Glass, 8), Weight),
                          Between(Whole, Channel(Glass, 0), Weight)};
    }
}

void PlanSpot(Ink &Out, const Moment &Now)
{
    const std::int32_t Cols = static_cast<std::int32_t>(Now.Cols), Rows = static_cast<std::int32_t>(Now.Rows);
    const std::int32_t Narrow = std::min(Cols, Rows * 2);
    Out.Spot = true;
    Out.Dark = Between(Whole, 14, Now.Mix());
    Out.SpotX = Cols / 2 + Sine(static_cast<std::uint32_t>(Now.PhaseMs * 1024u / 4300u)) * (Cols * 2 / 5) / 1024;
    Out.SpotY = Rows / 2 + Sine(static_cast<std::uint32_t>(Now.PhaseMs * 1024u / 3100u)) * (Rows * 2 / 5) / 1024;
    Out.SpotRadius = std::max(3, Narrow * (100 - static_cast<std::int32_t>(Now.Strength) * 40 / 255) / 300);
    Out.SpotEdge = std::max(2, Out.SpotRadius / 2);
    Out.SpotNear = static_cast<std::uint32_t>(Out.SpotRadius * Out.SpotRadius);
    Out.SpotFar = static_cast<std::uint32_t>((Out.SpotRadius + Out.SpotEdge) * (Out.SpotRadius + Out.SpotEdge));
}

void PlanFilm(Ink &Out, const Moment &Now)
{
    const std::uint32_t Cols = Now.Cols, Rows = Now.Rows;
    Out.Film = true;
    Out.Sepia = Now.Mix();
    Out.FilmScale = (256u << 16) / (Cols * Cols / 4 + Rows * Rows);
    const std::uint32_t Flicker = Whole - Hash(Now.Number * 40503u + 1u) % 28u * Out.Sepia / Whole;
    Out.Vignette.resize(257);
    for (std::uint32_t Place = 0; Place <= 256; Place++) {
        const std::uint32_t Edge = Whole - (Place * Place >> 8) * 150u / Whole;
        Out.Vignette[Place] = static_cast<std::uint16_t>(Between(Whole, Edge, Out.Sepia) * Flicker >> 8);
    }
    const std::uint32_t Roll = Hash(Now.Number / 2u * 9973u + 3u);
    if (Roll % 3u != 0) Out.Scratch[0] = static_cast<std::int32_t>((Roll >> 4) % Cols);
    if (Roll % 11u == 0) Out.Scratch[1] = static_cast<std::int32_t>((Roll >> 12) % Cols);
}

// What fade, invert, tint and the film's toning make of a colour: the same
// wherever it stands.
std::uint32_t Settled(const Ink &Plan, std::uint32_t Colour) noexcept
{
    std::uint32_t Red = Channel(Colour, 16), Green = Channel(Colour, 8), Blue = Channel(Colour, 0);
    if (Plan.Fade != 0) {
        const std::uint32_t Grey = (Red * 77 + Green * 151 + Blue * 28) >> 8;
        Red = Between(Red, Grey, Plan.Fade);
        Green = Between(Green, Grey, Plan.Fade);
        Blue = Between(Blue, Grey, Plan.Fade);
    }
    if (Plan.Invert != 0) {
        Red = Between(Red, 255 - Red, Plan.Invert);
        Green = Between(Green, 255 - Green, Plan.Invert);
        Blue = Between(Blue, 255 - Blue, Plan.Invert);
    }
    if (Plan.TintMix != 0) {
        Red = Red * Between(255, Channel(Plan.TintColor, 16), Plan.TintMix) / 255;
        Green = Green * Between(255, Channel(Plan.TintColor, 8), Plan.TintMix) / 255;
        Blue = Blue * Between(255, Channel(Plan.TintColor, 0), Plan.TintMix) / 255;
    }
    if (Plan.Sepia != 0) {
        const auto Toned = [&](std::uint32_t Of, std::uint32_t ByRed, std::uint32_t ByGreen, std::uint32_t ByBlue) {
            return Between(Of, std::min<std::uint32_t>(255, (Red * ByRed + Green * ByGreen + Blue * ByBlue) >> 8), Plan.Sepia);
        };
        return Pack(Toned(Red, 100, 197, 48), Toned(Green, 89, 175, 43), Toned(Blue, 69, 136, 34));
    }
    return Pack(Red, Green, Blue);
}

std::uint32_t Hued(const Hue &Turn, std::uint32_t Colour) noexcept
{
    const std::int32_t Red = static_cast<std::int32_t>(Channel(Colour, 16));
    const std::int32_t Green = static_cast<std::int32_t>(Channel(Colour, 8));
    const std::int32_t Blue = static_cast<std::int32_t>(Channel(Colour, 0));
    const auto Rotated = [](std::int32_t A, std::int32_t B, std::int32_t C, std::int32_t Red_, std::int32_t Green_,
                            std::int32_t Blue_, std::uint32_t Glass) {
        const std::int32_t Sum = std::clamp((A * Red_ + B * Green_ + C * Blue_) >> 10, 0, 255);
        return static_cast<std::uint32_t>(Sum) * Glass >> 8;
    };
    return Pack(Rotated(Turn.Diagonal, Turn.Minus, Turn.Plus, Red, Green, Blue, Turn.Red),
                Rotated(Turn.Plus, Turn.Diagonal, Turn.Minus, Red, Green, Blue, Turn.Green),
                Rotated(Turn.Minus, Turn.Plus, Turn.Diagonal, Red, Green, Blue, Turn.Blue));
}

// Whether a half of a cell the glass draws a half-block into, and its colours.
struct Halves {
    std::uint32_t Top, Bottom;
};

bool HalvesOf(const GlassCell &Cell, std::uint32_t Ink, std::uint32_t Paper, Halves &Out) noexcept
{
    const std::uint32_t Fg = Own(Cell.Cell.fg, Ink), Bg = Own(Cell.Cell.bg, Paper);
    switch (Cell.Cell.ch) {
    case UpperHalf: Out = {Fg, Bg}; return true;
    case LowerHalf: Out = {Bg, Fg}; return true;
    case Full:      Out = {Fg, Fg}; return true;
    case ' ':
    case 0:         Out = {Bg, Bg}; return true;
    default:        return false;
    }
}

void Plot(GlassCell &Cell, bool Lower, std::uint32_t Colour, std::uint32_t Weight, std::uint32_t Ink,
          std::uint32_t Paper) noexcept
{
    Halves Now;
    if (!HalvesOf(Cell, Ink, Paper, Now)) return;
    std::uint32_t &Half = Lower ? Now.Bottom : Now.Top;
    Half = Pack(Between(Channel(Half, 16), Channel(Colour, 16), Weight), Between(Channel(Half, 8), Channel(Colour, 8), Weight),
                Between(Channel(Half, 0), Channel(Colour, 0), Weight));
    Cell.Cell.ch = UpperHalf;
    Cell.Cell.fg = Now.Top;
    Cell.Cell.bg = Now.Bottom;
}

}  // namespace

void InkPlan(Ink &Out, const Moment &Now)
{
    const std::uint32_t Kinds = Now.Kinds;
    const std::uint32_t Mix = Now.Mix();
    Out.Fade = Out.Invert = Out.TintMix = Out.Sepia = 0;
    Out.Spot = Out.Film = Out.Rainbow = false;
    Out.Scratch[0] = Out.Scratch[1] = -1;
    Out.Dark = Whole;
    if (Kinds & DISPLAY_EFFECT_FADE) Out.Fade = Mix;
    if (Kinds & DISPLAY_EFFECT_INVERT) Out.Invert = Mix;
    if (Kinds & DISPLAY_EFFECT_TINT) {
        Out.TintMix = Mix;
        Out.TintColor = Now.Color;
    }
    if (Kinds & DISPLAY_EFFECT_RAINBOW) PlanRainbow(Out, Now);
    if (Kinds & DISPLAY_EFFECT_SPOTLIGHT) PlanSpot(Out, Now);
    if (Kinds & DISPLAY_EFFECT_SEPIA) PlanFilm(Out, Now);
    Out.Sparkle = (Kinds & DISPLAY_EFFECT_SHIMMER) ? 70u * Now.Level * Now.Strength / (255u * Whole) : 0;
    Out.Star = Brightened(Now.Color, 150);
    Out.Global = Out.Fade != 0 || Out.Invert != 0 || Out.TintMix != 0 || Out.Sepia != 0;
    Out.Any = Out.Global || Out.Rainbow || Out.Spot || Out.Film;
    if (Out.Global) {
        Out.Known.assign(1024, 0xFFFFFFFFu);
        Out.Learnt.resize(1024);
    }
}

std::uint32_t InkLight(const Ink &Plan, const Moment &Now, std::int32_t X, std::int32_t Y) noexcept
{
    std::uint32_t Light = Whole;
    if (Plan.Spot) {
        const std::int32_t DeltaX = X - Plan.SpotX, DeltaY = (Y - Plan.SpotY) * 2;
        const auto Far = static_cast<std::uint32_t>(DeltaX * DeltaX + DeltaY * DeltaY);
        if (Far >= Plan.SpotFar) Light = Plan.Dark;
        else if (Far > Plan.SpotNear)
            Light = Between(Whole, Plan.Dark, static_cast<std::uint32_t>((static_cast<std::int32_t>(Root(Far)) - Plan.SpotRadius) * 256 / Plan.SpotEdge));
    }
    if (Plan.Film) {
        const std::int32_t DeltaX = X - static_cast<std::int32_t>(Now.Cols) / 2;
        const std::int32_t DeltaY = (Y - static_cast<std::int32_t>(Now.Rows) / 2) * 2;
        const std::uint32_t Place = std::min<std::uint32_t>(256, static_cast<std::uint32_t>(DeltaX * DeltaX + DeltaY * DeltaY) * Plan.FilmScale >> 16);
        Light = Light * Plan.Vignette[Place] >> 8;
    }
    return Light;
}

std::uint32_t InkColour(Ink &Plan, std::uint32_t Colour, std::uint32_t Default, std::int32_t X,
                        std::uint32_t Light) noexcept
{
    Colour = Own(Colour, Default);
    if (Plan.Global) {
        const std::uint32_t Place = Colour * 2654435761u >> 22;
        if (Plan.Known[Place] != Colour) {
            Plan.Known[Place] = Colour;
            Plan.Learnt[Place] = Settled(Plan, Colour);
        }
        Colour = Plan.Learnt[Place];
    }
    if (Plan.Rainbow) Colour = Hued(Plan.Hues[static_cast<std::size_t>(X)], Colour);
    if (Light != Whole) Colour = Pack(Channel(Colour, 16) * Light >> 8, Channel(Colour, 8) * Light >> 8, Channel(Colour, 0) * Light >> 8);
    return Colour;
}

void InkScratch(GlassCell &Cell, std::uint32_t Default) noexcept
{
    Cell.Cell.fg = Brightened(Own(Cell.Cell.fg, Default), 70);
    Cell.Cell.bg = Brightened(Own(Cell.Cell.bg, Default), 70);
}

void InkSplit(GlassCell &Cell, const GlassCell &Left, const GlassCell &Right, std::uint32_t Ink,
              std::uint32_t Paper) noexcept
{
    const auto Apart = [](std::uint32_t Middle, std::uint32_t Before, std::uint32_t After) {
        return Pack(Channel(Before, 16), Channel(Middle, 8), Channel(After, 0));
    };
    Cell.Cell.fg = Apart(Own(Cell.Cell.fg, Ink), Own(Left.Cell.fg, Ink), Own(Right.Cell.fg, Ink));
    Cell.Cell.bg = Apart(Own(Cell.Cell.bg, Paper), Own(Left.Cell.bg, Paper), Own(Right.Cell.bg, Paper));
}

void InkSparkle(GlassCell &Cell, std::uint32_t Roll, std::uint32_t Star, std::uint32_t Ink) noexcept
{
    static constexpr char Stars[4] = {'*', '+', '.', static_cast<char>(0xFA)};
    if (Cell.Cell.ch == ' ' || Cell.Cell.ch == 0) {
        Cell.Cell.ch = Stars[(Roll >> 12) & 3u];
        Cell.Cell.fg = Star;
    } else {
        Cell.Cell.fg = Brightened(Own(Cell.Cell.fg, Ink), 190);
    }
}

// Two layers of flakes, each in a column of its own by chance: the far ones
// slow and faint, the near ones quick and bright, all swaying a little.
void InkSnow(std::vector<GlassCell> &Glass, const Moment &Now, std::uint32_t Ink, std::uint32_t Paper)
{
    static constexpr std::uint32_t Speeds[2] = {13, 27};
    static constexpr std::uint32_t Weights[2] = {120, 256};
    const std::uint32_t High = Now.Rows * 2;
    const std::uint32_t Density = 6u + 54u * Now.Mix() / Whole;
    const std::uint32_t Flake = Brightened(Now.Color, 200);
    for (std::uint32_t Layer = 0; Layer < 2; Layer++) {
        for (std::uint32_t X = 0; X < Now.Cols; X++) {
            const std::uint32_t Roll = Hash(X * 668265263u + Layer * 374761393u + 7u);
            if ((Roll & 255u) >= Density) continue;
            const std::uint32_t Fall = static_cast<std::uint32_t>(Now.PhaseMs * Speeds[Layer] / 1000u) + (Roll >> 8) % High;
            const std::uint32_t Pixel = Fall % High;
            const std::int32_t Sway = Sine(static_cast<std::uint32_t>(Now.PhaseMs * 1024u / 2600u) + (Roll >> 20) + Layer * 300u) * 2 / 1024;
            const std::int32_t Column = Wrap(static_cast<std::int32_t>(X) + Sway, static_cast<std::int32_t>(Now.Cols));
            GlassCell &Cell = Glass[(Pixel / 2) * Now.Cols + static_cast<std::uint32_t>(Column)];
            Plot(Cell, (Pixel & 1u) != 0, Flake, Weights[Layer], Ink, Paper);
        }
    }
}

}  // namespace effects
