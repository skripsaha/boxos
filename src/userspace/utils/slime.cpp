/* slime — a lazy yellow-green little creature that crawls over the console.
 *
 * The slime owns only its footprint. Every frame Painter paints the old small
 * rectangle away and lays the new one down; the rest of the console is never
 * cleared or redrawn. Its movement is deliberately slow and indecisive: it
 * chooses a new place, crawls there, pauses, and chooses again. */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "box/clock.h"
#include "box/cpu.h"
#include "box/form.h"
#include "box/print.h"

#include "painter/core/color.h"
#include "painter/tui/frame.h"
#include "painter/tui/keyboard.h"
#include "painter/tui/surface.h"

BOX_FORM("");

namespace {

using PainterColor = painter::core::Color;

constexpr PainterColor Black     = painter::core::Colors::Black;
constexpr PainterColor SlimeDark = PainterColor::Rgb(0x55, 0x86, 0x16);
constexpr PainterColor SlimeBase = PainterColor::Rgb(0x9F, 0xD7, 0x24);
constexpr PainterColor SlimeLight = PainterColor::Rgb(0xE0, 0xF5, 0x63);
constexpr PainterColor EyeWhite   = PainterColor::Rgb(0xF6, 0xF8, 0xD8);
constexpr PainterColor Pupil      = PainterColor::Rgb(0x13, 0x20, 0x0A);
/* Tune these values to make the creature lazier or more restless. */
constexpr float SlimeSpeedMin = 0.65f;
constexpr float SlimeSpeedMax = 1.35f;

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

std::uint64_t RandomState = 1;

float Unit()
{
    RandomState ^= RandomState << 13;
    RandomState ^= RandomState >> 7;
    RandomState ^= RandomState << 17;
    return static_cast<float>(RandomState >> 40) / static_cast<float>(1ull << 24);
}

float Between(float Low, float High)
{
    return Low + (High - Low) * Unit();
}

class Slime {
public:
    Slime(int ScreenWidth, int ScreenHeight, int BodyWidth, int BodyHeight)
        : ScreenWidth(ScreenWidth), ScreenHeight(ScreenHeight), BodyWidth(BodyWidth), BodyHeight(BodyHeight)
    {
        RandomState = cpu_rdtsc() | 1;
        X = Between(0.0f, static_cast<float>(ScreenWidth - BodyWidth));
        Y = Between(0.0f, static_cast<float>(ScreenHeight - BodyHeight));
        ChooseTarget();
    }

    void Step(float Seconds)
    {
        if (Pause > 0.0f) {
            Pause -= Seconds;
            return;
        }

        const float Dx = TargetX - X;
        const float Dy = TargetY - Y;
        const float Distance = std::sqrt(Dx * Dx + Dy * Dy);
        if (Distance < 0.15f) {
            X = TargetX;
            Y = TargetY;
            ChooseTarget();
            return;
        }

        const float Step = std::min(Distance, Speed * Seconds);
        X += Dx / Distance * Step;
        Y += Dy / Distance * Step;
        Phase += Seconds * (1.4f + Speed);
    }

    int Left() const { return static_cast<int>(std::lround(X)); }
    int Top() const { return static_cast<int>(std::lround(Y)); }
    float Wiggle() const { return std::sin(Phase) * 0.35f; }

    float LookX() const
    {
        return std::clamp(TargetX - X, -1.0f, 1.0f);
    }

    float LookY() const
    {
        return std::clamp(TargetY - Y, -1.0f, 1.0f);
    }

private:
    void ChooseTarget()
    {
        TargetX = Between(0.0f, static_cast<float>(ScreenWidth - BodyWidth));
        TargetY = Between(0.0f, static_cast<float>(ScreenHeight - BodyHeight));
        Speed = Between(SlimeSpeedMin, SlimeSpeedMax);
        Pause = Between(0.45f, 2.4f);
        if (Unit() < 0.28f) Pause = 0.0f;
    }

    int ScreenWidth;
    int ScreenHeight;
    int BodyWidth;
    int BodyHeight;
    float X = 0.0f;
    float Y = 0.0f;
    float TargetX = 0.0f;
    float TargetY = 0.0f;
    float Speed = 0.9f;
    float Pause = 0.0f;
    float Phase = 0.0f;
};

class SlimePicture {
public:
    SlimePicture(int Width, int Height, painter::tui::Frame Canvas)
        : Width(Width), Height(Height), PixelHeight(Height * 2), Canvas(std::move(Canvas))
    {
    }

    void Draw(const Slime &Creature)
    {
        Canvas.Clear({painter::core::Colors::LightGray, Black});
        const float CenterX = Width * 0.5f;
        const float CenterY = PixelHeight * 0.55f + Creature.Wiggle();
        const float RadiusX = std::max(2.0f, Width * 0.43f);
        const float RadiusY = std::max(2.0f, PixelHeight * 0.40f);

        for (int PixelY = 0; PixelY < PixelHeight; PixelY++) {
            for (int PixelX = 0; PixelX < Width; PixelX++) {
                const float Dx = (PixelX + 0.5f - CenterX) / RadiusX;
                const float Dy = (PixelY + 0.5f - CenterY) / RadiusY;
                float Shape = Dx * Dx + Dy * Dy;
                /* A slime has a heavier bottom and a soft, uneven crown. */
                if (Dy > 0.25f) Shape += (Dy - 0.25f) * 0.11f;
                if (Shape > 1.0f) continue;

                const float Highlight = std::clamp(0.68f - Dx * 0.25f - Dy * 0.42f, 0.0f, 1.0f);
                const PainterColor Base = PainterColor::Mix(
                    SlimeDark, SlimeLight, static_cast<std::uint8_t>(Highlight * 255.0f));
                Canvas.PutPixel({PixelX, PixelY}, Base);
            }
        }

        const float EyeY = CenterY - RadiusY * 0.28f;
        DrawEye(CenterX - RadiusX * 0.30f, EyeY, Creature.LookX(), Creature.LookY());
        DrawEye(CenterX + RadiusX * 0.30f, EyeY, Creature.LookX(), Creature.LookY());

        /* A small glint on the crown gives the body a wet, gelatinous read. */
        Canvas.PutPixel({static_cast<int>(CenterX - RadiusX * 0.32f),
                         static_cast<int>(CenterY - RadiusY * 0.66f)}, SlimeLight);
    }

    painter::tui::Frame &Frame() { return Canvas; }

private:
    void DrawEye(float CenterX, float CenterY, float LookX, float LookY)
    {
        const float EyeRadius = std::max(1.25f, Width * 0.075f);
        const float PupilRadius = std::max(0.55f, EyeRadius * 0.44f);
        for (int PixelY = static_cast<int>(CenterY - EyeRadius - 1);
             PixelY <= static_cast<int>(CenterY + EyeRadius + 1); PixelY++) {
            for (int PixelX = static_cast<int>(CenterX - EyeRadius - 1);
                 PixelX <= static_cast<int>(CenterX + EyeRadius + 1); PixelX++) {
                const float Dx = PixelX + 0.5f - CenterX;
                const float Dy = PixelY + 0.5f - CenterY;
                if (Dx * Dx + Dy * Dy <= EyeRadius * EyeRadius)
                    Canvas.PutPixel({PixelX, PixelY}, EyeWhite);
            }
        }

        const float PupilX = CenterX + std::clamp(LookX, -1.0f, 1.0f) * EyeRadius * 0.34f;
        const float PupilY = CenterY + std::clamp(LookY, -1.0f, 1.0f) * EyeRadius * 0.28f;
        for (int PixelY = static_cast<int>(PupilY - PupilRadius - 1);
             PixelY <= static_cast<int>(PupilY + PupilRadius + 1); PixelY++) {
            for (int PixelX = static_cast<int>(PupilX - PupilRadius - 1);
                 PixelX <= static_cast<int>(PupilX + PupilRadius + 1); PixelX++) {
                const float Dx = PixelX + 0.5f - PupilX;
                const float Dy = PixelY + 0.5f - PupilY;
                if (Dx * Dx + Dy * Dy <= PupilRadius * PupilRadius)
                    Canvas.PutPixel({PixelX, PixelY}, Pupil);
            }
        }
    }

    int Width;
    int Height;
    int PixelHeight;
    painter::tui::Frame Canvas;
};

}

int main()
{
    std::expected<painter::tui::Surface, painter::tui::StageError> Opened =
        painter::tui::Surface::Open("slime", 30, 10);
    if (!Opened) {
        Say(Opened.error().Describe("slime"));
        return 1;
    }
    painter::tui::Surface Screen = std::move(*Opened);
    const painter::core::Size Dimensions = Screen.Size();
    const int BodyWidth = std::min(16, std::max(10, Dimensions.Width / 5));
    const int BodyHeight = std::min(8, std::max(5, Dimensions.Height / 3));

    std::expected<painter::tui::Frame, box::error> Created =
        painter::tui::Frame::Create({BodyWidth, BodyHeight});
    if (!Created) {
        Say("slime: the creature could not be created");
        return 1;
    }
    Slime Creature(Dimensions.Width, Dimensions.Height, BodyWidth, BodyHeight);
    SlimePicture Picture(BodyWidth, BodyHeight, std::move(*Created));

    std::expected<painter::tui::Keyboard, painter::tui::StageError> Input =
        painter::tui::Keyboard::Open("slime");
    if (!Input) {
        Say(Input.error().Describe("slime"));
        return 1;
    }
    painter::tui::Keyboard Keys = std::move(*Input);

    int OldLeft = Creature.Left();
    int OldTop = Creature.Top();
    bool Shown = false;
    std::uint64_t Last = clock_uptime_us();
    for (;;) {
        const std::uint64_t Now = clock_uptime_us();
        const float Seconds = std::min(0.1f, static_cast<float>(Now - Last) / 1e6f);
        Last = Now;
        Creature.Step(Seconds);

        if (Shown) {
            Picture.Frame().Clear({painter::core::Colors::LightGray, Black});
            if (!Screen.Paint({OldLeft, OldTop}, Picture.Frame())) break;
        }

        Picture.Draw(Creature);
        const int Left = Creature.Left();
        const int Top = Creature.Top();
        if (!Screen.Paint({Left, Top}, Picture.Frame())) break;
        OldLeft = Left;
        OldTop = Top;
        Shown = true;

        const painter::tui::Event Event = Keys.Wait(33);
        if (Event.Kind == painter::tui::EventKind::Gone) break;
        if (Event.Kind == painter::tui::EventKind::Key && !Event.Key.IsExtended() &&
            (Event.Key.Ascii == 'q' || Event.Key.Ascii == 'Q' ||
             Event.Key.Ascii == painter::tui::KeyCodes::Escape))
            break;
    }

    if (Shown) {
        Picture.Frame().Clear({painter::core::Colors::LightGray, Black});
        (void)Screen.Paint({OldLeft, OldTop}, Picture.Frame());
    }
    (void)Keys.Close();
    (void)Screen.Close();
    Say("[SLIME] the creature went to sleep");
    return 0;
}
