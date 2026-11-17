/*
 * speed.cpp — how long the engine's work takes. See drawcheck.h.
 */

#include <chrono>
#include <cstdio>
#include <optional>
#include <vector>

#include "boxos_pixel.h"
#include "drawcheck.h"
#include "painter/core/cpu_path.h"

namespace drawcheck {

using namespace painter::core;
/* BoxLib's C colour type is named Color too: Painter's is meant here. */
using painter::core::Color;

void SaySpeed()
{
    constexpr std::uint32_t W = 1920, H = 1080;
    std::optional<Canvas>   C = Canvas::Make(W, H), Picture = Canvas::Make(W, H);
    std::vector<std::uint32_t> Glass(static_cast<std::size_t>(W) * H);
    if (!C || !Picture) {
        std::printf("drawcheck: speed not measured — no room for two canvases of 1920x1080\n");
        return;
    }
    const auto Took = [](auto &&Work) {
        const auto Start = std::chrono::steady_clock::now();
        Work();
        const double Ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - Start).count();
        return Ns / (static_cast<double>(W) * H);
    };
    const Path   Screen = Path::Rect({0, 0, W, H});
    const Path   Round  = Path::RoundedRect({60.5f, 40.25f, W - 120.0f, H - 80.0f}, Corners::All(48));
    const Fill   Across(Gradient::Linear({0, 0}, {W, H}, {{0, Color::Rgb(20, 24, 60)}, {1, Color::Rgb(200, 90, 120)}}));
    const Fill   FromMiddle(Gradient::Radial({W / 2.0f, H / 2.0f}, 900, {{0, Color::Rgb(250, 240, 220)}, {1, Color::Rgb(30, 20, 40)}}));
    Stroke       Ring;
    Ring.Width = 4;
    Picture->Fill(Screen, Across);
    const double Solid      = Took([&] { C->Fill(Screen, Fill(Color::Rgb(30, 30, 40))); });
    const double SeeThrough     = Took([&] { C->Fill(Round, Fill(Color::Rgba(250, 250, 255, 128))); });
    const double Along    = Took([&] { C->Fill(Screen, Across); });
    const double Out    = Took([&] { C->Fill(Screen, FromMiddle); });
    const double Lined      = Took([&] { C->Stroke(Round, Ring, Fill(Color::Rgb(255, 255, 255))); });
    const double Laid      = Took([&] { C->Draw(*Picture, Point{0, 0}, 0.6f); });
    const double Shown     = Took([&] { C->CopyOut(Rect{0, 0, static_cast<std::int32_t>(W), static_cast<std::int32_t>(H)}, Glass.data(), W, BOX_PIXEL_BGR, Dither::Off); });
    const double Dithered  = Took([&] { C->CopyOut(Rect{0, 0, static_cast<std::int32_t>(W), static_cast<std::int32_t>(H)}, Glass.data(), W, BOX_PIXEL_BGR, Dither::On); });
    std::printf("drawcheck: %s, ns a pixel of 1920x1080 — fill %.2f, see-through %.2f, gradient %.2f, radial %.2f, "
                "ring %.2f, canvas laid %.2f, shown %.2f, dithered %.2f\n",
                CpuPathName(ChosenCpuPath()), Solid, SeeThrough, Along, Out, Lined, Laid, Shown, Dithered);
}

}  // namespace drawcheck
