/* donut — a small rotating torus painted over the console.
 *
 *   donut                    right middle, unless words choose its place
 *   donut top left           top left
 *   donut middle             centred
 *   finish donut             it stops
 *
 * It lives on by itself, as eyes and the clock do (box/life.h stays): it
 * starts turning at once, whoever starts it gets the prompt back, and it asks
 * for no key — nothing is waited for and nothing ends it but finish.
 *
 * The torus is a real depth-tested surface, sampled from its two parameters
 * and projected into Painter's two-pixel cells. It owns only its rectangle:
 * the surrounding console remains untouched while it turns. This is the same
 * regional contract as eyes, so a person can place both programs independently
 * without one taking the whole screen from the other. Its colour, speed and
 * size are the person's (settings donut), changed while it turns. */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "box/clock.h"
#include "box/cpu.h"
#include "box/form.h"
#include "box/life.h"
#include "box/print.h"
#include "box/settings.h"
#include "box/cxx/settings.h"

#include "painter/core/color.h"
#include "painter/tui/frame.h"
#include "painter/tui/surface.h"

BOX_FORM("takes optional words \"where it stands: top, bottom, left, right or "
         "middle; right middle if none\"\n");
BOX_LIFE("stays");
BOX_SETTINGS("speed number from 20 to 300 percent default 100 \"how fast the donut turns\"\n"
             "size number from 40 to 200 percent default 100 \"how large the donut is\"\n"
             "color color default #FFFFFF \"the donut's light side: its far side is a shade of it\"\n");

namespace {

using PainterColor = painter::core::Color;

constexpr float Pi = 3.14159265358979323846f;
constexpr PainterColor Black = painter::core::Colors::Black;
/* The zone is deliberately larger than the visible torus. Perspective makes
 * its footprint grow while it turns, so a tight frame would clip the rim. */
constexpr int DonutZoneMaxWidth = 42;
constexpr int DonutZoneMaxHeight = 24;
constexpr float DonutScale = 0.28f;
constexpr std::uint32_t TurningMs = 50;
constexpr float Major = 1.0f;
constexpr float Minor = 0.43f;
/* Samples to a pixel along the ring and the tube, with the nearest part of
 * the torus drawn a third larger than its middle by perspective. */
constexpr float Density = 2.6f;

/* What the person set, read again whenever the file is saved. */
struct Look {
    float Speed = 1.0f;
    float Size = 1.0f;
    PainterColor Light = painter::core::Colors::White;
};

Look ReadLook() {
    using box::settings;
    return Look{.Speed = static_cast<float>(settings::number("speed")) / 100.0f,
                .Size = static_cast<float>(settings::number("size")) / 100.0f,
                .Light = PainterColor::FromRaw(settings::color("color"))};
}

void Say(std::string_view Text) {
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

struct Place {
    int Row = 0;
    int Col = 0;
};

Place Where(const char *const *Words, std::size_t Count, int Cols, int Rows,
            int Wide, int High) {
    int Across = Count == 0 ? 2 : 1;
    int Down = 1;
    bool Horizontal = false;
    bool Vertical = false;
    for (std::size_t At = 0; At < Count; At++) {
        const std::string_view Word = Words[At];
        if (Word == "left") {
            Across = 0;
            Horizontal = true;
        }
        if (Word == "right") {
            Across = 2;
            Horizontal = true;
        }
        if (Word == "top") {
            Down = 0;
            Vertical = true;
        }
        if (Word == "bottom") {
            Down = 2;
            Vertical = true;
        }
        if (Word == "middle" || Word == "centre" || Word == "center") {
            if (!Horizontal)
                Across = 1;
            if (!Vertical)
                Down = 1;
        }
    }

    Place Result;
    const int MaxCol = std::max(0, Cols - Wide);
    const int MaxRow = std::max(0, Rows - High);
    Result.Col = Across == 0 ? 0 : Across == 1 ? MaxCol / 2 : MaxCol;
    Result.Row = Down == 0 ? std::min(2, MaxRow) : Down == 1 ? MaxRow / 2 : MaxRow;
    return Result;
}

class DonutPicture {
  public:
    DonutPicture(int Width, int Height, painter::tui::Frame Canvas)
        : Width(Width), Height(Height), PixelHeight(Height * 2),
          Depth(static_cast<std::size_t>(Width) * PixelHeight),
          Canvas(std::move(Canvas)) {
        const float Size = static_cast<float>(std::min(Width, PixelHeight)) * DonutScale;
        const int Rings = std::clamp(static_cast<int>(2.0f * Pi * (Major + Minor) * Size * Density), 96, 480);
        const int Tubes = std::clamp(static_cast<int>(2.0f * Pi * Minor * Size * Density), 32, 160);
        for (int Index = 0; Index < Rings; Index++) {
            const float U = 2.0f * Pi * static_cast<float>(Index) / Rings;
            RingCos.push_back(std::cos(U));
            RingSin.push_back(std::sin(U));
        }
        for (int Index = 0; Index < Tubes; Index++) {
            const float V = 2.0f * Pi * static_cast<float>(Index) / Tubes;
            TubeCos.push_back(std::cos(V));
            TubeSin.push_back(std::sin(V));
        }
    }

    void Colour(PainterColor Light) {
        Low = Light;
        High = PainterColor::Mix(Light, Black, 136);
    }

    bool Draw(float A, float B) {
        Canvas.ClearTransparent();
        std::fill(Depth.begin(), Depth.end(),
                  std::numeric_limits<float>::lowest());

        const float Ca = std::cos(A);
        const float Sa = std::sin(A);
        const float Cb = std::cos(B);
        const float Sb = std::sin(B);
        constexpr float Camera = 3.3f;
        const float Scale =
            static_cast<float>(std::min(Width, PixelHeight)) * DonutScale;

        for (std::size_t UIndex = 0; UIndex < RingCos.size(); UIndex++) {
            const float Cu = RingCos[UIndex];
            const float Su = RingSin[UIndex];
            for (std::size_t VIndex = 0; VIndex < TubeCos.size(); VIndex++) {
                const float Cv = TubeCos[VIndex];
                const float Sv = TubeSin[VIndex];

                const float Nx = Cv * Cu;
                const float Ny = Cv * Su;
                const float Nz = Sv;
                const float Nzx = Ny * Sa + Nz * Ca;
                const float Nzy = -Nx * Sb + Nzx * Cb;
                if (Nzy < -0.15f)
                    continue;

                const float Radius = Major + Minor * Cv;
                const float X = Radius * Cu;
                const float Y = Radius * Su;
                const float Z = Minor * Sv;
                const float Yx = Y * Ca - Z * Sa;
                const float Zx = Y * Sa + Z * Ca;
                const float Nyx = Ny * Ca - Nz * Sa;
                const float Xy = X * Cb + Zx * Sb;
                const float Zy = -X * Sb + Zx * Cb;
                const float Nxy = Nx * Cb + Nzx * Sb;

                const float Perspective = Camera / (Camera - Zy);
                const int PixelX =
                    static_cast<int>(Width * 0.5f + Xy * Scale * Perspective);
                const int PixelY = static_cast<int>(PixelHeight * 0.5f -
                                                    Yx * Scale * Perspective);
                if (PixelX < 0 || PixelX >= Width || PixelY < 0 ||
                    PixelY >= PixelHeight)
                    continue;

                const std::size_t At =
                    static_cast<std::size_t>(PixelY) * Width + PixelX;
                if (Zy <= Depth[At])
                    continue;
                Depth[At] = Zy;

                const float Light =
                    0.18f +
                    std::max(0.0f, Nxy * 0.25f - Nyx * 0.35f + Nzy * 0.9f) *
                        0.82f;
                Canvas.PutPixel({PixelX, PixelY}, Ink(Sv, Light));
            }
        }
        return true;
    }

    painter::tui::Frame &Frame() { return Canvas; }

  private:
    PainterColor Ink(float V, float Light) const {
        const float Wrapped = std::clamp((V + 1.0f) * 0.5f, 0.0f, 1.0f);
        const PainterColor Base = PainterColor::Mix(
            Low, High, static_cast<std::uint8_t>(Wrapped * 255.0f));
        return PainterColor::Mix(
            Black, Base,
            static_cast<std::uint8_t>(std::clamp(Light, 0.0f, 1.0f) * 255.0f));
    }

    PainterColor Low = painter::core::Colors::White;
    PainterColor High = PainterColor::Mix(painter::core::Colors::White, Black, 136);
    std::vector<float> RingCos, RingSin, TubeCos, TubeSin;
    int Width;
    int Height;
    int PixelHeight;
    std::vector<float> Depth;
    painter::tui::Frame Canvas;
};

struct Zone {
    int Width;
    int Height;
};

Zone ZoneFor(painter::core::Size Dimensions, float Size) {
    return {std::clamp(static_cast<int>(DonutZoneMaxWidth * Size), 8, std::max(8, Dimensions.Width / 3)),
            std::clamp(static_cast<int>(DonutZoneMaxHeight * Size), 5, std::max(5, Dimensions.Height / 2))};
}

} // namespace

int main() {
    const char *const *Words = nullptr;
    std::size_t Count = 0;
    if (form_words(0, &Words, &Count) != 0)
        return 1;

    std::expected<painter::tui::Surface, painter::tui::StageError> Opened =
        painter::tui::Surface::Open(30, 10, painter::tui::SurfaceRole::Notice);
    if (!Opened) {
        Say(Opened.error().Describe("donut"));
        return 1;
    }
    painter::tui::Surface Screen = std::move(*Opened);
    const painter::core::Size Dimensions = Screen.Size();

    Look Set = ReadLook();
    Zone Area = ZoneFor(Dimensions, Set.Size);
    Place At = Where(Words, Count, Dimensions.Width, Dimensions.Height, Area.Width, Area.Height);
    std::expected<painter::tui::Frame, box::error> Created =
        painter::tui::Frame::Create({Area.Width, Area.Height});
    if (!Created) {
        Say("donut: the picture could not be created");
        return 1;
    }
    auto Picture = std::make_unique<DonutPicture>(Area.Width, Area.Height, std::move(*Created));
    Picture->Colour(Set.Light);
    box::settings::watch Watch;

    // Turning until it can no more — the display gone, or the donut ended:
    // nothing but its end stops it.
    std::uint64_t Last = clock_uptime_us();
    float A = 0.0f;
    float B = 0.0f;
    for (;;) {
        if (Watch.changed()) {
            Set = ReadLook();
            const Zone Fresh = ZoneFor(Dimensions, Set.Size);
            std::expected<painter::tui::Frame, box::error> Resized =
                painter::tui::Frame::Create({Fresh.Width, Fresh.Height});
            if (!Resized)
                break;
            At = Where(Words, Count, Dimensions.Width, Dimensions.Height, Fresh.Width, Fresh.Height);
            Picture = std::make_unique<DonutPicture>(Fresh.Width, Fresh.Height, std::move(*Resized));
            Picture->Colour(Set.Light);
        }
        const std::uint64_t Now = clock_uptime_us();
        const float Seconds =
            std::min(0.1f, static_cast<float>(Now - Last) / 1e6f);
        Last = Now;
        A += Seconds * 1.05f * Set.Speed;
        B += Seconds * 0.73f * Set.Speed;
        Picture->Draw(A, B);
        if (!Screen.Clear() || !Screen.Composite({At.Col, At.Row}, Picture->Frame()))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(TurningMs));
    }
    (void)Screen.Close();
    return 1;
}
