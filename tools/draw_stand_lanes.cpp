/*
 * draw stand — the ways the engine's rows are worked (painter/src/core/lanes.h):
 * the dither table held to light.h for every light and threshold; SSE2 and
 * AVX2 held to the plain way bit for bit, on rows of every length, every
 * share and every pixel a canvas can hold, and on rows of gradients and
 * pictures every way they are seen; and how long each takes.
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
#include "light.h"
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

/* A number of 64 bits, of up to Bits of them, either sign. */
std::int64_t RandomOf(std::uint32_t Bits)
{
    const std::uint64_t Wide = (std::uint64_t{Random()} << 32 | Random()) & ((std::uint64_t{1} << Bits) - 1u);
    return Random() % 2u ? static_cast<std::int64_t>(Wide) : -static_cast<std::int64_t>(Wide);
}

/* Every way colours rows of gradients and smooth pictures as the plain way
 * does (paint_rows.h): linear and radial ones — from their middle, the way
 * a vector works, and from a focal point — and conic, every extend, near
 * their ends and far past them; pictures of every size, read within and
 * past their edges every way, seen at every opacity. */
void CheckPaintRows(Tally &Count, const std::vector<Way> &All)
{
    std::uint32_t Rows = 0, Wrong = 0;
    std::vector<Pixel> Table(BOX_PAINT_GRADIENT / 16 + 2);
    for (Pixel &Each : Table) Each = RandomPixel();
    const BoxPaintStop Stop{0, 0};
    for (std::uint32_t I = 0; I < 60000u; I++) {
        const std::uint32_t Length = Random() % 70u;
        BoxPaint            P{};
        P.Extend    = Random() % 4u;
        P.Stops     = &Stop;
        P.StopCount = 1;
        const std::uint32_t Kind = Random() % 4u;
        P.Kind   = Kind == 0 ? BOX_PAINT_LINEAR : Kind == 3 ? BOX_PAINT_CONIC : BOX_PAINT_RADIAL;
        P.FocalX = Kind == 2 ? static_cast<std::int32_t>(RandomOf(15)) : 0;
        P.FocalY = Kind == 2 ? static_cast<std::int32_t>(RandomOf(15)) : 0;
        /* Sums of a place a pixel's sixteen bits of rounding below, from near
         * the gradient's span to far past what a screen shows. */
        const std::uint32_t Reach = 20u + Random() % 36u;
        const lanes::GradientWalk Walk{&P, Table.data(), RandomOf(Reach), RandomOf(Reach), RandomOf(Reach - 4u), RandomOf(Reach - 4u)};
        std::vector<Pixel> Want(Length);
        for (std::size_t W = 0; W < All.size(); W++) {
            std::vector<Pixel> Got(Length);
            All[W].Kit->Gradient(Got.data(), Walk, Length);
            Rows++;
            if (W == 0) Want = Got;
            else if (std::memcmp(Got.data(), Want.data(), Length * sizeof(Pixel)) != 0) Wrong++;
        }
    }
    for (std::uint32_t I = 0; I < 60000u; I++) {
        const std::uint32_t Length = Random() % 70u;
        BoxPaint            P{};
        P.Kind          = BOX_PAINT_PICTURE;
        P.Extend        = Random() % 4u;
        P.PictureWidth  = 1u + Random() % 40u;
        P.PictureHeight = 1u + Random() % 40u;
        P.PictureStride = P.PictureWidth + Random() % 3u;
        std::vector<Pixel> Texels(static_cast<std::size_t>(P.PictureStride) * P.PictureHeight + 1u);
        for (Pixel &Each : Texels) Each = RandomPixel();
        /* Places from a few pixels before the picture to a few past it, or
         * now and then far away; steps of up to two pixels a pixel. */
        const auto Place = [&](std::uint32_t Side) {
            if (Random() % 16u == 0) return RandomOf(52);
            return (static_cast<std::int64_t>(Random() % ((Side + 6u) * BOX_SHAPE_ONE)) - 3 * BOX_SHAPE_ONE) * BOX_SHAPE_UNIT + RandomOf(16);
        };
        const lanes::PictureWalk Walk{&P, Texels.data(), Place(P.PictureWidth), Place(P.PictureHeight), RandomOf(26), RandomOf(26),
                                      Random() % 3u ? 65535u : static_cast<std::uint32_t>(Random() % 65536u)};
        std::vector<Pixel> Want(Length);
        for (std::size_t W = 0; W < All.size(); W++) {
            std::vector<Pixel> Got(Length);
            All[W].Kit->Picture(Got.data(), Walk, Length);
            Rows++;
            if (W == 0) Want = Got;
            else if (std::memcmp(Got.data(), Want.data(), Length * sizeof(Pixel)) != 0) Wrong++;
        }
    }
    char Line[200];
    std::snprintf(Line, sizeof(Line),
                  "SSE2 and AVX2 colour every row of a gradient or a smooth picture the very bits the plain way does: "
                  "%u rows, %u not alike",
                  Rows, Wrong);
    Count.Check(Wrong == 0, Line);
}

}  // namespace

void CheckLanes(Tally &Count)
{
    /* The table the glass's numbers are looked up in: every light, at every
     * threshold, the number light.h works out. */
    {
        const light::Steps       &S     = light::StepsOfLight();
        const light::DitherTable *Table = light::DitherTableOfLight();
        std::uint32_t             Wrong = Table ? 0u : 1u;
        for (std::uint32_t Light = 0; Table && Light < 65536u; Light++)
            for (std::uint32_t Threshold = 0; Threshold < 256u; Threshold++)
                Wrong += light::DitheredBy(Table->Of[Light], Threshold) != light::Dithered(Light, S, Threshold) ? 1u : 0u;
        char Line[200];
        std::snprintf(Line, sizeof(Line),
                      "the dither table gives every light at every threshold the number light.h works out: 65536 x 256, "
                      "%u not alike",
                      Wrong);
        Count.Check(Wrong == 0, Line);
    }

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
        /* A third of the rows a picture laid whole, mostly whole itself —
         * the run the kits copy rather than work out — a pixel in sixteen
         * not, so a run of them is broken where it would be. */
        const bool Solid = I % 3u == 0;
        for (std::uint32_t K = 0; K < Length; K++) {
            Below[K]      = RandomPixel();
            Paint[K]      = RandomPixel();
            Shares[K]     = RandomShare();
            Thresholds[K] = static_cast<std::uint8_t>(Random());
            if (Solid && Random() % 16u)
                Paint[K] = Pixel{static_cast<std::uint16_t>(Random()), static_cast<std::uint16_t>(Random()),
                                 static_cast<std::uint16_t>(Random()), 65535};
        }
        const Pixel         Color = RandomPixel();
        const std::uint32_t One   = Solid ? 65535u : RandomShare();
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
    CheckPaintRows(Count, All);
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
