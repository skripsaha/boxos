/* crab — a large, friendly crab crossing the glass. */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

#include "box/clock.h"
#include "box/error.h"
#include "box/form.h"
#include "box/print.h"

#include "painter/core/color.h"
#include "painter/tui/frame.h"
#include "painter/tui/glyph.h"
#include "painter/tui/keyboard.h"
#include "painter/tui/surface.h"

/* The crab takes no command words; q or Escape leaves it. */
BOX_FORM("");

namespace {

using Color = painter::core::Color;
namespace Palette {
constexpr Color Outline  = Color::Rgb(0x4A, 0x12, 0x18);
constexpr Color Shell    = Color::Rgb(0xC8, 0x32, 0x3C);
constexpr Color ShellHot = Color::Rgb(0xF0, 0x65, 0x5A);
constexpr Color ShellLow = Color::Rgb(0x8A, 0x1D, 0x2C);
constexpr Color Leg       = Color::Rgb(0xA8, 0x32, 0x28);
constexpr Color Sclera    = Color::Rgb(0xFF, 0xF0, 0xC8);
constexpr Color Pupil     = Color::Rgb(0x18, 0x20, 0x38);
constexpr Color Glint     = Color::Rgb(0xFF, 0xFF, 0xFF);
constexpr Color Shadow    = Color::Rgb(0x42, 0x16, 0x20);
}  // namespace Palette

constexpr std::int32_t SpriteWidth  = 34;
constexpr std::int32_t SpriteHeight = 14;
constexpr float        Speed        = 13.0f;
constexpr float        Pi           = 3.14159265358979323846f;

char MirrorGlyph(char Glyph, bool Left)
{
    if (!Left) return Glyph;
    if (Glyph == '/') return '\\';
    if (Glyph == '\\') return '/';
    if (Glyph == '<') return '>';
    if (Glyph == '>') return '<';
    return Glyph;
}

void Put(painter::tui::Frame &Canvas, std::int32_t X, std::int32_t Y, char Glyph,
         Color Foreground, Color Background, bool Left = false)
{
    if (X < 0 || Y < 0 || X >= SpriteWidth || Y >= SpriteHeight) return;
    const std::int32_t Column = Left ? SpriteWidth - 1 - X : X;
    Canvas.Put({Column, Y}, painter::tui::Cell{MirrorGlyph(Glyph, Left), Foreground, Background});
}

void Block(painter::tui::Frame &Canvas, std::int32_t Left, std::int32_t Top,
          std::int32_t Width, std::int32_t Height, Color Ink, bool FacingLeft)
{
    for (std::int32_t Row = 0; Row < Height; Row++)
        for (std::int32_t Col = 0; Col < Width; Col++)
            Put(Canvas, Left + Col, Top + Row, painter::tui::Glyphs::Full, Ink, Ink, FacingLeft);
}

class Crab {
public:
    void Step(float Seconds, std::int32_t ScreenWidth)
    {
        Time += Seconds;
        X += (FacingLeft ? -Speed : Speed) * Seconds;
        const float Limit = std::max(0.0f, static_cast<float>(ScreenWidth - SpriteWidth));
        if (X <= 0.0f) {
            X = 0.0f;
            FacingLeft = false;
        } else if (X >= Limit) {
            X = Limit;
            FacingLeft = true;
        }
    }

    std::int32_t XPosition() const noexcept { return static_cast<std::int32_t>(X); }
    bool          FacingLeftNow() const noexcept { return FacingLeft; }
    float         TimeNow() const noexcept { return Time; }

private:
    float X = 0.0f;
    float Time = 0.0f;
    bool  FacingLeft = false;
};

void DrawCrab(const Crab &Creature, painter::tui::Frame &Canvas)
{
    Canvas.ClearTransparent();
    const bool Left = Creature.FacingLeftNow();
    const int  Step = static_cast<int>(Creature.TimeNow() * 7.0f) & 1;
    const int  Look = static_cast<int>(std::sin(Creature.TimeNow() * 1.7f) > 0.0f);

    /* A low shadow gives the body weight without painting a rectangle behind it. */
    for (int Col = 8; Col < 27; Col += 2)
        Put(Canvas, Col, 12 + ((Col / 2) & 1), painter::tui::Glyphs::LightShade,
            Palette::Shadow, Palette::Shadow, Left);

    /* Eight legs, alternating their angle as the crab walks. */
    constexpr int LegRoots[] = {10, 13, 20, 23};
    for (int Index = 0; Index < 4; Index++) {
        const int Root = LegRoots[Index];
        const bool Down = ((Index + Step) & 1) != 0;
        Put(Canvas, Root, 9, Down ? '/' : '\\', Palette::Leg, Palette::Outline, Left);
        Put(Canvas, Root - 1, 10, Down ? '/' : '\\', Palette::Leg, Palette::Outline, Left);
        Put(Canvas, Root - 2, 11, '-', Palette::Leg, Palette::Outline, Left);
        Put(Canvas, SpriteWidth - 1 - Root, 9, Down ? '\\' : '/', Palette::Leg, Palette::Outline, Left);
        Put(Canvas, SpriteWidth - Root, 10, Down ? '\\' : '/', Palette::Leg, Palette::Outline, Left);
        Put(Canvas, SpriteWidth + 1 - Root, 11, '-', Palette::Leg, Palette::Outline, Left);
    }

    /* Claws open and close in opposite rhythm to the feet. */
    Put(Canvas, 3, 5, '<', Palette::ShellHot, Palette::Outline, Left);
    Put(Canvas, 4, 4, '/', Palette::Shell, Palette::Outline, Left);
    Put(Canvas, 5, 6, '\\', Palette::ShellLow, Palette::Outline, Left);
    Put(Canvas, 29, 5, '>', Palette::ShellHot, Palette::Outline, Left);
    Put(Canvas, 28, 4, '\\', Palette::Shell, Palette::Outline, Left);
    Put(Canvas, 27, 6, '/', Palette::ShellLow, Palette::Outline, Left);
    if (Step == 0) {
        Put(Canvas, 2, 4, '<', Palette::ShellHot, Palette::Outline, Left);
        Put(Canvas, 31, 4, '>', Palette::ShellHot, Palette::Outline, Left);
    }

    /* Carapace: dark outline, warm shell, and a few deliberate highlights. */
    Block(Canvas, 10, 5, 14, 5, Palette::Outline, Left);
    Block(Canvas, 8, 6, 18, 3, Palette::Shell, Left);
    Block(Canvas, 11, 5, 12, 1, Palette::ShellHot, Left);
    Block(Canvas, 11, 9, 12, 1, Palette::ShellLow, Left);
    Put(Canvas, 12, 6, painter::tui::Glyphs::Dot, Palette::ShellHot, Palette::Shell, Left);
    Put(Canvas, 16, 7, painter::tui::Glyphs::Dot, Palette::ShellHot, Palette::Shell, Left);
    Put(Canvas, 20, 6, painter::tui::Glyphs::Dot, Palette::ShellHot, Palette::Shell, Left);

    /* Stalks and expressive eyes. The dot glyph on a cream cell reads as a pupil. */
    Put(Canvas, 13, 4, '|', Palette::Outline, Palette::Shell, Left);
    Put(Canvas, 20, 4, '|', Palette::Outline, Palette::Shell, Left);
    for (int Eye : {12, 19}) {
        Put(Canvas, Eye, 2, ' ', Palette::Sclera, Palette::Sclera, Left);
        Put(Canvas, Eye + 1, 2, ' ', Palette::Sclera, Palette::Sclera, Left);
        const int PupilColumn = Eye + (Look ? 1 : 0);
        Put(Canvas, PupilColumn, 2, painter::tui::Glyphs::Dot, Palette::Pupil, Palette::Sclera, Left);
        Put(Canvas, Eye + (Look ? 0 : 1), 2, painter::tui::Glyphs::Dot,
            Palette::Glint, Palette::Sclera, Left);
    }
    Put(Canvas, 12, 1, '^', Palette::Outline, Palette::Outline, Left);
    Put(Canvas, 19, 1, '^', Palette::Outline, Palette::Outline, Left);
}

}  // namespace

int main()
{
    std::expected<painter::tui::Surface, painter::tui::StageError> Opened =
        painter::tui::Surface::Open("crab", SpriteWidth + 2, SpriteHeight + 2);
    if (!Opened) {
        print_to_screen((Opened.error().Describe("crab") + '\n').c_str());
        return 1;
    }
    painter::tui::Surface Screen = std::move(*Opened);
    const painter::core::Size Dimensions = Screen.Size();
    if (Dimensions.Width < SpriteWidth || Dimensions.Height < SpriteHeight) {
        print_to_screen("crab: the screen is too small for the crab\n");
        return 1;
    }

    std::expected<painter::tui::Frame, box::error> Created =
        painter::tui::Frame::Create({SpriteWidth, SpriteHeight});
    if (!Created) return 1;
    painter::tui::Frame Canvas = std::move(*Created);

    std::expected<painter::tui::Keyboard, painter::tui::StageError> Input =
        painter::tui::Keyboard::Open("crab");
    if (!Input) {
        print_to_screen((Input.error().Describe("crab") + '\n').c_str());
        return 1;
    }
    painter::tui::Keyboard Keys = std::move(*Input);
    Crab Creature;
    std::uint64_t Last = clock_uptime_us();

    for (;;) {
        const std::uint64_t Now = clock_uptime_us();
        const float Seconds = std::min(0.1f, static_cast<float>(Now - Last) / 1e6f);
        Last = Now;
        Creature.Step(Seconds, Dimensions.Width);
        DrawCrab(Creature, Canvas);
        const int Top = std::max(0, (Dimensions.Height - SpriteHeight) / 2);
        if (!Screen.Clear() || !Screen.Composite({Creature.XPosition(), Top}, Canvas)) break;

        const painter::tui::Event Event = Keys.Wait(33);
        if (Event.Kind == painter::tui::EventKind::Gone) break;
        if (Event.Kind == painter::tui::EventKind::Key && !Event.Key.IsExtended() &&
            (Event.Key.Ascii == 'q' || Event.Key.Ascii == 'Q' ||
             Event.Key.Ascii == painter::tui::KeyCodes::Escape))
            break;
    }

    (void)Screen.Clear();
    (void)Keys.Close();
    (void)Screen.Close();
    print_to_screen("[CRAB] scuttled away\n");
    return 0;
}
