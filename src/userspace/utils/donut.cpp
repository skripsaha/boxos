/* donut — a small rotating torus painted over the console.
 *
 *   donut                    bottom right, unless words choose its place
 *   donut top left           top left
 *   donut middle             centred
 *   q, ESC                   leave
 *
 * The torus is a real depth-tested surface, sampled from its two parameters
 * and projected into Painter's two-pixel cells. It owns only its rectangle:
 * the surrounding console remains untouched while it turns. This is the same
 * regional contract as eyes, so a person can place both programs independently
 * without one taking the whole screen from the other.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "box/clock.h"
#include "box/cpu.h"
#include "box/form.h"
#include "box/print.h"

#include "painter/core/color.h"
#include "painter/tui/frame.h"
#include "painter/tui/keyboard.h"
#include "painter/tui/surface.h"

BOX_FORM("takes optional words \"where it stands: top, bottom, left, right or middle; right middle if none\"\n");

namespace {

using PainterColor = painter::core::Color;

constexpr float Pi = 3.14159265358979323846f;
constexpr PainterColor Black = painter::core::Colors::Black;
/* Change these two colors to retheme the torus. */
constexpr PainterColor DonutColorLow = PainterColor::Rgb(0x1C, 0x63, 0xC6);
constexpr PainterColor DonutColorHigh = PainterColor::Rgb(0xF0, 0x83, 0x2E);
constexpr int DonutMaxWidth = 28;
constexpr int DonutMaxHeight = 14;

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

struct Place {
    int Row = 0;
    int Col = 0;
};

Place Where(const char *const *Words, std::size_t Count, int Cols, int Rows, int Wide, int High)
{
    int Across = 2;
    int Down = 1;
    for (std::size_t At = 0; At < Count; At++) {
        const std::string_view Word = Words[At];
        if (Word == "left") Across = 0;
        if (Word == "right") Across = 2;
        if (Word == "top") Down = 0;
        if (Word == "bottom") Down = 2;
        if (Word == "middle" || Word == "centre" || Word == "center") {
            bool SaidSide = false;
            for (std::size_t Other = 0; Other < Count; Other++) {
                const std::string_view Side = Words[Other];
                SaidSide = SaidSide || Side == "left" || Side == "right" ||
                           Side == "top" || Side == "bottom";
            }
            if (!SaidSide) Across = Down = 1;
        }
    }
    if (Count == 1 && (std::string_view(Words[0]) == "top" ||
                       std::string_view(Words[0]) == "bottom"))
        Across = 1;
    if (Count == 1 && (std::string_view(Words[0]) == "left" ||
                       std::string_view(Words[0]) == "right"))
        Down = 1;

    Place Result;
    Result.Col = Across == 0 ? 2 : Across == 1 ? (Cols - Wide) / 2 : Cols - Wide - 2;
    Result.Row = Down == 0 ? 2 : Down == 1 ? (Rows - High) / 2 : Rows - High - 1;
    return Result;
}

PainterColor SurfaceInk(float V, float Light)
{
    const float Wrapped = std::clamp((V + 1.0f) * 0.5f, 0.0f, 1.0f);
    const PainterColor Base = PainterColor::Mix(
        DonutColorLow, DonutColorHigh, static_cast<std::uint8_t>(Wrapped * 255.0f));
    return PainterColor::Mix(Black, Base,
                             static_cast<std::uint8_t>(std::clamp(Light, 0.0f, 1.0f) * 255.0f));
}

class DonutPicture {
public:
    DonutPicture(int Width, int Height, painter::tui::Frame Canvas)
        : Width(Width), Height(Height), PixelHeight(Height * 2),
          Depth(static_cast<std::size_t>(Width) * PixelHeight),
          Canvas(std::move(Canvas))
    {
    }

    bool Draw(float A, float B)
    {
        Canvas.Clear({painter::core::Colors::LightGray, Black});
        std::fill(Depth.begin(), Depth.end(), std::numeric_limits<float>::lowest());

        const float Ca = std::cos(A);
        const float Sa = std::sin(A);
        const float Cb = std::cos(B);
        const float Sb = std::sin(B);
        constexpr float Major = 1.0f;
        constexpr float Minor = 0.43f;
        constexpr float Camera = 3.3f;
        const float Scale = static_cast<float>(std::min(Width, PixelHeight)) * 0.43f;

        constexpr int UCount = 112;
        constexpr int VCount = 56;
        for (int UIndex = 0; UIndex < UCount; UIndex++) {
            const float U = 2.0f * Pi * static_cast<float>(UIndex) / UCount;
            const float Cu = std::cos(U);
            const float Su = std::sin(U);
            for (int VIndex = 0; VIndex < VCount; VIndex++) {
                const float V = 2.0f * Pi * static_cast<float>(VIndex) / VCount;
                const float Cv = std::cos(V);
                const float Sv = std::sin(V);

                const float Radius = Major + Minor * Cv;
                const float X = Radius * Cu;
                const float Y = Radius * Su;
                const float Z = Minor * Sv;
                const float Nx = Cv * Cu;
                const float Ny = Cv * Su;
                const float Nz = Sv;

                const float Yx = Y * Ca - Z * Sa;
                const float Zx = Y * Sa + Z * Ca;
                const float Nyx = Ny * Ca - Nz * Sa;
                const float Nzx = Ny * Sa + Nz * Ca;
                const float Xy = X * Cb + Zx * Sb;
                const float Zy = -X * Sb + Zx * Cb;
                const float Nxy = Nx * Cb + Nzx * Sb;
                const float Nzy = -Nx * Sb + Nzx * Cb;

                const float Perspective = Camera / (Camera - Zy);
                const int PixelX = static_cast<int>(Width * 0.5f + Xy * Scale * Perspective);
                const int PixelY = static_cast<int>(PixelHeight * 0.5f - Yx * Scale * Perspective);
                if (PixelX < 0 || PixelX >= Width || PixelY < 0 || PixelY >= PixelHeight)
                    continue;

                const std::size_t At = static_cast<std::size_t>(PixelY) * Width + PixelX;
                if (Zy <= Depth[At]) continue;
                Depth[At] = Zy;

                const float Light = 0.18f +
                                    std::max(0.0f, Nxy * 0.25f - Nyx * 0.35f + Nzy * 0.9f) * 0.82f;
                Canvas.PutPixel({PixelX, PixelY}, SurfaceInk(Sv, Light));
            }
        }
        return true;
    }

    painter::tui::Frame &Frame() { return Canvas; }

private:
    int Width;
    int Height;
    int PixelHeight;
    std::vector<float> Depth;
    painter::tui::Frame Canvas;
};

}

int main()
{
    const char *const *Words = nullptr;
    std::size_t Count = 0;
    if (form_words(0, &Words, &Count) != 0) return 1;

    std::expected<painter::tui::Surface, painter::tui::StageError> Opened =
        painter::tui::Surface::Open("donut", 30, 10);
    if (!Opened) {
        Say(Opened.error().Describe("donut"));
        return 1;
    }
    painter::tui::Surface Screen = std::move(*Opened);
    const painter::core::Size Dimensions = Screen.Size();
    const int Width = std::min(DonutMaxWidth, std::max(8, Dimensions.Width - 4));
    const int Height = std::min(DonutMaxHeight, std::max(5, Dimensions.Height - 4));
    const Place At = Where(Words, Count, Dimensions.Width, Dimensions.Height, Width, Height);

    std::expected<painter::tui::Frame, box::error> Created =
        painter::tui::Frame::Create({Width, Height});
    if (!Created) {
        Say("donut: the picture could not be created");
        return 1;
    }

    std::expected<painter::tui::Keyboard, painter::tui::StageError> Input =
        painter::tui::Keyboard::Open("donut");
    if (!Input) {
        Say(Input.error().Describe("donut"));
        return 1;
    }
    painter::tui::Keyboard Keys = std::move(*Input);
    DonutPicture Picture(Width, Height, std::move(*Created));

    std::uint64_t Last = clock_uptime_us();
    float A = 0.0f;
    float B = 0.0f;
    for (;;) {
        const std::uint64_t Now = clock_uptime_us();
        const float Seconds = std::min(0.1f, static_cast<float>(Now - Last) / 1e6f);
        Last = Now;
        A += Seconds * 1.05f;
        B += Seconds * 0.73f;
        Picture.Draw(A, B);
        if (!Screen.Paint({At.Col, At.Row}, Picture.Frame())) break;

        const painter::tui::Event Event = Keys.Wait(33);
        if (Event.Kind == painter::tui::EventKind::Gone) break;
        if (Event.Kind == painter::tui::EventKind::Key && !Event.Key.IsExtended() &&
            (Event.Key.Ascii == 'q' || Event.Key.Ascii == 'Q' ||
             Event.Key.Ascii == painter::tui::KeyCodes::Escape))
            break;
    }

    Picture.Frame().Clear({painter::core::Colors::LightGray, Black});
    (void)Screen.Paint({At.Col, At.Row}, Picture.Frame());
    (void)Keys.Close();
    (void)Screen.Close();
    Say(std::format("[DONUT] rotation stopped"));
    return 0;
}
