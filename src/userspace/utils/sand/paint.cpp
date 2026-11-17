/* paint.cpp — the picture of the sandbox. */

#include "paint.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <string>
#include <utility>

namespace sand {

namespace {

namespace tui = painter::tui;
using painter::core::Colors::Black;
using painter::core::Colors::White;

const Color BarPaper = Color::Rgb(0x2A, 0x2A, 0x2A);
const Color BarInk   = Color::Rgb(0xEE, 0xEE, 0xEE);
const Color KeyInk   = painter::core::Colors::Amber;
const Color HintInk  = Color::Rgb(0x9A, 0x9A, 0x9A);

// A grain's own shade: lighter or darker than its kind's color by up to Variety percent.
Color Grained(Color Base, std::uint8_t Grain, int Variety)
{
    const int Swing = std::abs(Grain - 128) * 2 * Variety * 2 / 500;
    return Color::Between(Base, Grain >= 128 ? White : Black, static_cast<std::uint8_t>(Swing));
}

// A flame is white at its hottest, the flame's color in the middle and dark as it dies;
// a burning thing glows in its own color through the fire.
Color Flame(const Look &Shown, const Cell &Burning)
{
    if (Burning.Life >= FuelFloor)
        return Grained(Color::Over(Shown.Colors[static_cast<int>(Kind::Fire)],
                                  Shown.Colors[static_cast<int>(Burning.Was)], 90),
                       Burning.Grain, Shown.Variety);
    const int   Heat = std::min<int>(Burning.Life, 28) * 255 / 28;
    const Color Fire = Shown.Colors[static_cast<int>(Kind::Fire)];
    const Color Lit  = Heat > 170 ? Color::Between(Fire, White, static_cast<std::uint8_t>((Heat - 170) * 2))
                                  : Color::Between(Black, Fire, static_cast<std::uint8_t>(Heat * 255 / 170));
    return Grained(Lit, Burning.Grain, Shown.Variety);
}

// Whether the cell above, away from where things fall, is open air: the surface
// of a liquid catches the light.
bool Surface(const World &Field, int X, int Y)
{
    return Field.Inside(X, Y - Field.Fall) && Field.At(X, Y - Field.Fall).Type == Kind::Air;
}

Color CellColor(const Sandbox &Box, int X, int Y)
{
    const Look &Shown = Box.Shown;
    const Cell &Here  = Box.Field.At(X, Y);
    const Color Base  = Shown.Colors[static_cast<int>(Here.Type)];
    switch (Here.Type) {
    case Kind::Air:   return Base;
    case Kind::Fire:  return Flame(Shown, Here);
    case Kind::Smoke:
    case Kind::Steam:
        return Color::Over(Shown.Colors[static_cast<int>(Kind::Air)], Grained(Base, Here.Grain, Shown.Variety),
                          static_cast<std::uint8_t>(std::min(255, Here.Life * 2)));
    case Kind::Lava:
    case Kind::Water:
    case Kind::Oil: {
        const int Variety = Here.Type == Kind::Lava ? std::min(100, Shown.Variety * 2) : Shown.Variety;
        const Color Body  = Grained(Base, Here.Grain, Variety);
        return Surface(Box.Field, X, Y) ? Color::Between(Body, White, 70) : Body;
    }
    default:          return Grained(Base, Here.Grain, Shown.Variety);
    }
}

// The brush's outline: the cells under it that have a neighbor that is not.
void Ring(std::vector<Color> &Pixels, const Sandbox &Box)
{
    const World &Field = Box.Field;
    const int    Weight = Box.Pours ? 220 : 120;
    for (int Dy = -Box.Brush; Dy <= Box.Brush; Dy++) {
        for (int Dx = -Box.Brush; Dx <= Box.Brush; Dx++) {
            const int X = Box.CursorX + Dx, Y = Box.CursorY + Dy;
            if (!Field.Inside(X, Y) || !InBrush(Dx, Dy, Box.Brush)) continue;
            const bool Edge = !InBrush(Dx + 1, Dy, Box.Brush) || !InBrush(Dx - 1, Dy, Box.Brush) ||
                              !InBrush(Dx, Dy + 1, Box.Brush) || !InBrush(Dx, Dy - 1, Box.Brush);
            if (!Edge) continue;
            Color &Pixel = Pixels[static_cast<std::size_t>(Y) * Field.Width + X];
            Pixel        = Color::Over(Pixel, Box.Shown.Cursor, static_cast<std::uint8_t>(Weight));
        }
    }
}

// Black or white lettering, whichever reads on `Ground`.
Color Lettering(Color Ground)
{
    const int Light = (Ground.Red() * 3 + Ground.Green() * 6 + Ground.Blue()) / 10;
    return Light > 140 ? Black : White;
}

std::string Status(const Sandbox &Box)
{
    const char *Name = Box.Chosen == EraserAt ? "eraser" : TraitOf(Box.Material()).Name;
    std::string Said = std::format("{}  brush {}  {}/s", Name, Box.Brush, Box.Speed);
    if (Box.Weather == Sky::Rain) Said += "  rain";
    if (Box.Weather == Sky::Sandstorm) Said += "  sandstorm";
    if (Box.Upward) Said += "  gravity up";
    if (Box.Paused) Said += "  paused";
    else if (!Box.Pours) Said += "  enter pours";
    return Said;
}

void Bar(tui::Frame &Canvas, const Sandbox &Box)
{
    const int Row = Canvas.Size().Height - 1;
    Canvas.Fill({0, Row, Canvas.Size().Width, 1}, ' ', tui::TextStyle{BarInk, BarPaper});
    int Col = 0;
    for (int At = 0; At <= EraserAt; At++) {
        const Color Ground = At == EraserAt ? HintInk : Box.Shown.Colors[static_cast<int>(Pourable[static_cast<std::size_t>(At)])];
        const char  Key    = At == EraserAt ? 'e' : static_cast<char>(At == 9 ? '0' : '1' + At);
        const bool  Picked = At == Box.Chosen;
        const char  Label[4] = {Picked ? '[' : ' ', Key, Picked ? ']' : ' ', '\0'};
        Canvas.Write({Col, Row}, Label, tui::TextStyle{Lettering(Ground), Ground});
        Col += 3;
    }
    Canvas.Write({Col + 1, Row}, Status(Box), tui::TextStyle{BarInk, BarPaper});
}

void Help(tui::Frame &Canvas, const Sandbox &Box)
{
    const auto Spelled = [&](SandKey Which) {
        char Chord[KEY_CHORD_SPELLED_MAX];
        KeyChordSpell(&Box.Shown.Keys[Which], Chord, sizeof Chord);
        return std::string(Chord);
    };
    const std::pair<std::string, const char *> Lines[] = {
        {"arrows", "move the brush; held, faster"},
        {"1-9 0", "pick a material"},
        {"tab", "the next material"},
        {"enter", "pour, or stop pouring"},
        {"e", "the eraser"},
        {"space", "pause; drawing still works"},
        {"+ -", "a larger, a smaller brush"},
        {"w", "weather: rain, sandstorm, none"},
        {"c", "clear everything"},
        {"r", "the scene again"},
        {Spelled(KeyGravity), "turn gravity over"},
        {Spelled(KeyFaster), "more steps in a second"},
        {Spelled(KeySlower), "fewer steps in a second"},
        {Spelled(KeyHelp), "this page"},
        {"esc q", "leave"},
    };
    constexpr int KeyColumn = 13, TextColumn = 31;
    const int     Wide = 2 + KeyColumn + TextColumn, High = static_cast<int>(std::size(Lines)) + 2;
    const int     Left = std::max(0, (Canvas.Size().Width - Wide) / 2);
    const int     Top  = std::max(0, (Canvas.Size().Height - High) / 2);
    Canvas.Fill({Left, Top, Wide, High}, ' ', tui::TextStyle{BarInk, BarPaper});
    Canvas.Write({Left + 2, Top}, "sand", tui::TextStyle{KeyInk, BarPaper});
    for (std::size_t At = 0; At < std::size(Lines); At++) {
        const int Row = Top + 1 + static_cast<int>(At);
        Canvas.Write({Left + 2, Row}, Lines[At].first, tui::TextStyle{KeyInk, BarPaper});
        Canvas.Write({Left + 2 + KeyColumn, Row}, Lines[At].second, tui::TextStyle{BarInk, BarPaper});
    }
}

}  // namespace

void Picture::Draw(tui::Stage &Screen, const Sandbox &Box)
{
    tui::Frame &Canvas = Screen.FrameBuffer();
    const World &Field = Box.Field;
    Pixels.assign(static_cast<std::size_t>(Canvas.Size().Width) * Canvas.PixelHeight(),
                  Box.Shown.Colors[static_cast<int>(Kind::Air)]);
    for (int Y = 0; Y < Field.Height; Y++)
        for (int X = 0; X < Field.Width; X++) Pixels[static_cast<std::size_t>(Y) * Field.Width + X] = CellColor(Box, X, Y);
    Ring(Pixels, Box);
    Canvas.PutPixels(Pixels);
    Bar(Canvas, Box);
    if (Box.Helping) Help(Canvas, Box);
}

}  // namespace sand
