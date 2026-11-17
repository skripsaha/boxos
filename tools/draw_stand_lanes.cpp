/*
 * draw stand — the ways the engine's rows are worked (painter/src/core/lanes.h):
 * SSE2 and AVX2 held to the plain way bit for bit, on rows of every length,
 * every share and every pixel a canvas can hold; and how long each takes.
 * On a host that is not x86-64 only the plain way is there — the stand built
 * for x86-64 holds the others.
 */

#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

#include "draw_stand.h"

#include "boxos_pixel.h"
#include "lanes.h"
#include "painter/core/canvas.h"

namespace stand {

using namespace painter::core;
using painter::core::Color;

namespace {

struct Way {
    const char       *Name;
    const lanes::Kit *Kit;
};

std::vector<Way> Ways()
{
    std::vector<Way> All = {{"plain", &lanes::Plain()}};
#if defined(__x86_64__)
    All.push_back({"SSE2", &lanes::Sse2()});
    All.push_back({"AVX2", &lanes::Avx2()});
#endif
    return All;
}

/* A pixel a canvas can hold: light no more than its opacity. */
Pixel RandomPixel()
{
    const std::uint32_t Pick = Random() % 8u;
    const std::uint16_t A    = Pick == 0 ? 0 : Pick == 1 ? 65535 : static_cast<std::uint16_t>(Random());
    const auto          Upto = [&] { return A ? static_cast<std::uint16_t>(Random() % (A + 1u)) : std::uint16_t{0}; };
    return Pixel{Upto(), Upto(), Upto(), A};
}

std::uint16_t RandomShare()
{
    const std::uint32_t Pick = Random() % 6u;
    return Pick == 0 ? 0 : Pick == 1 ? 65535 : static_cast<std::uint16_t>(Random());
}

}  // namespace

void CheckLanes(Tally &Count)
{
    const std::vector<Way> All = Ways();
    if (All.size() == 1) {
        Count.Check(true, "the plain way alone on this host: the stand built for x86-64 holds SSE2 and AVX2 to it");
        return;
    }
    std::uint32_t Rows = 0, Wrong = 0;
    for (std::uint32_t I = 0; I < 40000u; I++) {
        const std::uint32_t        Length = Random() % 70u;
        std::vector<Pixel>         Below(Length), Paint(Length);
        std::vector<std::uint16_t> Shares(Length);
        std::vector<std::uint8_t>  Thresholds(Length);
        for (std::uint32_t K = 0; K < Length; K++) {
            Below[K]      = RandomPixel();
            Paint[K]      = RandomPixel();
            Shares[K]     = RandomShare();
            Thresholds[K] = static_cast<std::uint8_t>(Random());
        }
        const Pixel         Color = RandomPixel();
        const std::uint32_t One   = RandomShare();
        const std::uint32_t Format = Random() % 2u ? BOX_PIXEL_RGB : BOX_PIXEL_BGR;
        std::vector<Pixel>         Want[3];
        std::vector<std::uint32_t> Shown[2];
        for (std::size_t W = 0; W < All.size(); W++) {
            std::vector<Pixel> Got[3] = {Below, Below, Below};
            All[W].Kit->LayColor(Got[0].data(), Shares.data(), Length, Color);
            All[W].Kit->LayPaint(Got[1].data(), Paint.data(), Shares.data(), Length);
            All[W].Kit->LayRow(Got[2].data(), Paint.data(), Length, One);
            std::vector<std::uint32_t> Out[2] = {std::vector<std::uint32_t>(Length), std::vector<std::uint32_t>(Length)};
            All[W].Kit->Show(Out[0].data(), Below.data(), Length, Format, nullptr);
            All[W].Kit->Show(Out[1].data(), Below.data(), Length, Format, Thresholds.data());
            Rows++;
            if (W == 0) {
                for (int K = 0; K < 3; K++) Want[K] = Got[K];
                Shown[0] = Out[0], Shown[1] = Out[1];
            } else if (Got[0] != Want[0] || Got[1] != Want[1] || Got[2] != Want[2] || Out[0] != Shown[0] ||
                       Out[1] != Shown[1]) {
                Wrong++;
            }
        }
    }
    char Line[200];
    std::snprintf(Line, sizeof(Line),
                  "SSE2 and AVX2 work every row the very bits the plain way does — laid, painted, laid whole, shown "
                  "near and dithered: %u rows, %u not alike",
                  Rows, Wrong);
    Count.Check(Wrong == 0, Line);
}

void SaySpeed()
{
    using Clock = std::chrono::steady_clock;
    const auto Took = [](auto &&Work, std::uint64_t Pixels) {
        const auto Start = Clock::now();
        Work();
        const double Ns = std::chrono::duration<double, std::nano>(Clock::now() - Start).count();
        return Ns / static_cast<double>(Pixels);
    };
    const std::uint32_t        N = 1920u * 1080u;
    std::vector<Pixel>         Row(N), Paint(N);
    std::vector<std::uint16_t> Full(N, 65535), Part(N);
    std::vector<std::uint32_t> Out(N);
    std::vector<std::uint8_t>  Thresholds(N);
    for (std::uint32_t I = 0; I < N; I++) {
        Row[I] = RandomPixel(), Paint[I] = RandomPixel(), Part[I] = static_cast<std::uint16_t>(Random() | 1u);
        Thresholds[I] = static_cast<std::uint8_t>(Random());
    }
    std::printf("draw speed: ns a pixel, a 1920x1080 frame's worth each\n");
    for (const Way &W : Ways()) {
        const Pixel Opaque{40000, 30000, 20000, 65535};
        std::printf("  %-5s lay opaque %.2f  lay part %.2f  paint %.2f  laid whole %.2f  show %.2f  show dithered %.2f\n",
                    W.Name, Took([&] { W.Kit->LayColor(Row.data(), Full.data(), N, Opaque); }, N),
                    Took([&] { W.Kit->LayColor(Row.data(), Part.data(), N, Opaque); }, N),
                    Took([&] { W.Kit->LayPaint(Row.data(), Paint.data(), Part.data(), N); }, N),
                    Took([&] { W.Kit->LayRow(Row.data(), Paint.data(), N, 40000); }, N),
                    Took([&] { W.Kit->Show(Out.data(), Row.data(), N, BOX_PIXEL_BGR, nullptr); }, N),
                    Took([&] { W.Kit->Show(Out.data(), Row.data(), N, BOX_PIXEL_BGR, Thresholds.data()); }, N));
    }
    std::optional<Canvas> C = Canvas::Make(1920, 1080);
    const Path            Screen = Path::Rect({0, 0, 1920, 1080});
    const Path            Round  = Path::RoundedRect({100.5f, 80.25f, 1700, 900}, Corners::All(64));
    const Fill            Sky(Gradient::Linear({0, 0}, {0, 1080}, {{0, Color::Rgb(20, 24, 60)}, {1, Color::Rgb(90, 40, 80)}}));
    std::printf("  canvas, the way chosen: fill %.2f  gradient %.2f  rounded, soft %.2f  ring 4 wide %.2f\n",
                Took([&] { C->Fill(Screen, Fill(Color::Rgb(30, 30, 40))); }, N),
                Took([&] { C->Fill(Screen, Sky); }, N),
                Took([&] { C->Fill(Round, Fill(Color::Rgba(250, 250, 255, 128))); }, N),
                Took([&] {
                    Stroke Line;
                    Line.Width = 4;
                    C->Stroke(Round, Line, Fill(Colors::White));
                }, N));
}

}  // namespace stand
