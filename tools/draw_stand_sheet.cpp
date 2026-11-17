/*
 * draw stand — a sheet of everything the engine draws, to look at: rounded
 * cards with their shadows raised and pressed, gradients of every kind and
 * every Extend, strokes of every cap, join and dash, a clip, a picture sharp
 * and smooth, frosted glass, written dithered as a PPM.
 */

#include <cstdio>
#include <optional>
#include <vector>

#include "draw_stand.h"

#include "boxos_pixel.h"
#include "painter/core/canvas.h"

namespace stand {

using namespace painter::core;
using painter::core::Color;

bool WriteSheet(const char *Out)
{
    const std::uint32_t   W = 1280, H = 800;
    std::optional<Canvas> C = Canvas::Make(W, H);
    if (!C) return false;
    C->Fill(Path::Rect({0, 0, W, H}),
            Fill(Gradient::Linear({0, 0}, {0, H}, {{0, Color::Rgb(18, 20, 44)}, {1, Color::Rgb(44, 22, 48)}})));

    /* Cards: raised, pressed, and one of two fills. */
    const Path Card = Path::RoundedRect({40, 40, 220, 140}, Corners{24, 24, 8, 8});
    Shadow     Raised;
    Raised.Offset = {0, 10};
    Raised.Blur   = 24;
    Raised.Color  = Color::Rgba(0, 0, 0, 150);
    C->DrawShadow(*Raised.MaskFor(Card), Raised);
    const Fill Plashka[2] = {
        Fill(Gradient::Linear({0, 40}, {0, 180}, {{0, Color::Rgb(232, 120, 160)}, {1, Color::Rgb(190, 70, 120)}})),
        Fill(Gradient::Radial({110, 60}, 120, {{0, Color::Rgba(255, 255, 255, 110)}, {1, Color::Rgba(255, 255, 255, 0)}}))};
    C->Fill(Card, std::span<const Fill>(Plashka, 2));
    Shadow Pressed;
    Pressed.Inner  = true;
    Pressed.Offset = {0, 6};
    Pressed.Blur   = 10;
    Pressed.Color  = Color::Rgba(0, 0, 0, 170);
    const Path Well = Path::RoundedRect({300, 40, 220, 140}, Corners::All(70));
    C->Fill(Well, Fill(Color::Rgb(60, 66, 100)));
    C->DrawShadow(*Pressed.MaskFor(Well), Pressed);

    /* Gradients: radial from a focal point, conic, and the three Extends. */
    C->Fill(Path::Circle({620, 110}, 70), Fill(Gradient::Radial({620, 110}, 70, {590, 80},
                                                                 {{0, Color::Rgb(255, 245, 250)}, {0.6f, Color::Rgb(220, 150, 210)}, {1, Color::Rgb(120, 70, 170)}})));
    C->Fill(Path::Circle({780, 110}, 70),
            Fill(Gradient::Conic({780, 110}, 0, {{0, Color::Rgb(255, 80, 80)}, {0.33f, Color::Rgb(80, 255, 120)}, {0.66f, Color::Rgb(80, 120, 255)}, {1, Color::Rgb(255, 80, 80)}})));
    const Extend Pasts[3] = {Extend::Pad, Extend::Repeat, Extend::Reflect};
    for (int K = 0; K < 3; K++)
        C->Fill(Path::Rect({880.0f, 40.0f + 50.0f * K, 360, 40}),
                Fill(Gradient::Linear({1000, 0}, {1060, 0}, {{0, Color::Rgb(250, 210, 90)}, {1, Color::Rgb(40, 120, 200)}}, Pasts[K])));

    /* Strokes: caps, joins, dashes, dots, aligned. */
    const LineJoin Joins[3] = {LineJoin::Miter, LineJoin::Round, LineJoin::Bevel};
    const LineCap  Caps[3]  = {LineCap::Butt, LineCap::Round, LineCap::Square};
    for (int K = 0; K < 3; K++) {
        Stroke Line;
        Line.Width = 14;
        Line.Join  = Joins[K];
        Line.Cap   = Caps[K];
        const float X = 60.0f + 160.0f * K;
        C->Stroke(Path().MoveTo({X, 330}).LineTo({X + 50, 240}).LineTo({X + 100, 330}), Line, Fill(Color::Rgb(240, 240, 250)));
    }
    Stroke Dashed;
    Dashed.Width  = 6;
    Dashed.Cap    = LineCap::Round;
    Dashed.Dashes = {18, 12};
    C->Stroke(Path::RoundedRect({560, 240, 200, 100}, Corners::All(30)), Dashed, Fill(Color::Rgb(120, 220, 200)));
    Stroke Dots;
    Dots.Width  = 8;
    Dots.Cap    = LineCap::Round;
    Dots.Dashes = {0, 16};
    C->Stroke(*Path::FromSvg("M 800 330 C 850 230 950 230 1000 330 S 1150 430 1200 330"), Dots,
              Fill(Color::Rgb(250, 200, 120)));
    for (int K = 0; K < 3; K++) {
        Stroke Edge;
        Edge.Width = 10;
        Edge.Align = static_cast<StrokeAlign>(K);
        const Path Box = Path::Rect({60.0f + 120.0f * K, 400, 80, 60});
        C->Fill(Box, Fill(Color::Rgb(70, 80, 130)));
        C->Stroke(Box, Edge, Fill(Color::Rgba(255, 255, 255, 180)));
    }

    /* A clip: stripes seen through a star. */
    const Path Star = *Path::FromSvg("M 520 390 L 548 456 L 620 460 L 565 505 L 584 575 L 520 535 L 456 575 L 475 505 L 420 460 L 492 456 Z");
    C->PushClip(Star);
    for (int K = 0; K < 12; K++)
        C->Fill(Path::Rect({400.0f + 20.0f * K, 380, 10, 200}), Fill(Color::Rgb(250, 230, 120)));
    C->PopClip();

    /* A picture: sharp four times larger, smooth turned. */
    std::vector<std::uint8_t> Rgba(16u * 16u * 4u);
    for (std::uint32_t Y = 0; Y < 16u; Y++)
        for (std::uint32_t X = 0; X < 16u; X++) {
            std::uint8_t *P = &Rgba[(Y * 16u + X) * 4u];
            P[0] = static_cast<std::uint8_t>(X * 16), P[1] = static_cast<std::uint8_t>(Y * 16), P[2] = 200;
            P[3] = (X + Y) % 2 ? 255 : 160;
        }
    const auto Shown = Picture::FromRgba(Rgba, 16, 16, 64);
    C->Fill(Path::Rect({680, 400, 64, 64}), Fill::Picture(Shown, Transform::Scale(4).Then(Transform::Translate(680, 400)), Extend::None, true));
    C->Fill(Path::Rect({0, 0, 16, 16}), Fill::Picture(Shown),
            Transform::Scale(4).Then(Transform::Rotate(20)).Then(Transform::Translate(820, 390)));

    /* Frosted glass: the stripes behind a pane blurred, the pane laid over. */
    for (int K = 0; K < 10; K++)
        C->Fill(Path::Rect({960.0f + 28.0f * K, 400, 14, 160}), Fill(Color::Rgb(K % 2 ? 250 : 80, 120, K % 2 ? 90 : 230)));
    const Path Pane = Path::RoundedRect({1000, 430, 200, 100}, Corners::All(18));
    C->PushClip(Pane);
    C->Blur(Rect{990, 420, 220, 120}, 16);
    C->PopClip();
    C->Fill(Pane, Fill(Color::Rgba(255, 255, 255, 60)));
    C->Stroke(Pane, Stroke{}, Fill(Color::Rgba(255, 255, 255, 140)));

    /* A dark gradient wide enough to band in eight bits. */
    C->Fill(Path::Rect({40, 620, 1200, 140}),
            Fill(Gradient::Linear({40, 0}, {1240, 0}, {{0, Color::Rgb(14, 14, 18)}, {1, Color::Rgb(30, 30, 36)}})));

    std::vector<std::uint32_t> Glass(static_cast<std::size_t>(W) * H);
    C->CopyOut(Rect{0, 0, static_cast<std::int32_t>(W), static_cast<std::int32_t>(H)}, Glass.data(), W, BOX_PIXEL_BGR, Dither::On);
    std::FILE *File = std::fopen(Out, "wb");
    if (!File) return false;
    std::fprintf(File, "P6\n%u %u\n255\n", W, H);
    for (std::uint32_t Pixel : Glass) {
        const unsigned char Rgb[3] = {static_cast<unsigned char>(Pixel >> 16), static_cast<unsigned char>(Pixel >> 8),
                                      static_cast<unsigned char>(Pixel)};
        std::fwrite(Rgb, 1, 3, File);
    }
    return std::fclose(File) == 0;
}

}  // namespace stand
