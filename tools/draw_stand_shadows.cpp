/*
 * draw stand — shadows and blur (painter/core/shadow.h, Mask::Blur,
 * Canvas::Blur): a sharp shadow its shape moved; a blur keeping the light it
 * spreads and its symmetry; spread growing and shrinking a shape by its
 * width; an inner shadow only within its shape, darker toward the light; a
 * canvas blurred keeping its mean and its flat colours.
 */

#include <cmath>
#include <cstdio>
#include <optional>

#include "draw_stand.h"

#include "painter/core/canvas.h"

namespace stand {

using namespace painter::core;
using painter::core::Color;

namespace {

double Sum(const Mask &M)
{
    double Total = 0;
    for (std::int32_t Y = M.Area().Y; Y < M.Area().Y + M.Area().Height; Y++)
        for (std::int32_t X = M.Area().X; X < M.Area().X + M.Area().Width; X++) Total += M.At(X, Y) / 255.0;
    return Total;
}

std::string Said(const char *Format, double A, double B = 0)
{
    char Line[240];
    std::snprintf(Line, sizeof(Line), Format, A, B);
    return Line;
}

}  // namespace

void CheckShadows(Tally &Count)
{
    const Path Card = Path::RoundedRect({40, 30, 120, 80}, Corners::All(12));

    /* Sharp: the shape's own coverage, moved. */
    {
        Shadow Cast;
        Cast.Offset = {3, 4};
        const std::optional<Mask> Kept = Cast.MaskFor(Card);
        const std::optional<Mask> Want = Mask::Of(Card, Transform::Translate(3, 4), FillRule::NonZero,
                                                  Rect{-1000, -1000, 2000, 2000});
        bool Same = Kept && Want && Kept->Area() == Want->Area();
        for (std::int32_t Y = 0; Same && Y < 200; Y++)
            for (std::int32_t X = 0; X < 220; X++) Same = Same && Kept->At(X, Y) == Want->At(X, Y);
        Count.Check(Same, "a shadow with no blur is its shape's coverage, moved by its offset");
    }

    /* Blurred: the light it spreads kept, and its symmetry. */
    {
        Shadow Cast;
        Cast.Blur                       = 16;
        const Path                Disc  = Path::Circle({100, 100}, 30);
        const std::optional<Mask> Kept  = Cast.MaskFor(Disc);
        const std::optional<Mask> Sharp = Mask::Of(Disc, {}, FillRule::NonZero, Rect{0, 0, 200, 200});
        std::int32_t              Worst = 0;
        for (std::int32_t Y = 40; Y < 160; Y++)
            for (std::int32_t X = 40; X < 160; X++) {
                Worst = std::max(Worst, std::abs(Kept->At(X, Y) - Kept->At(199 - X, Y)));
                Worst = std::max(Worst, std::abs(Kept->At(X, Y) - Kept->At(Y, X)));
            }
        Count.Check(Near(Sum(*Kept), Sum(*Sharp), 0.01) && Worst <= 1 && Kept->At(100, 100) == 255 &&
                        Kept->At(100, 130) > 90 && Kept->At(100, 130) < 165,
                    Said("a shadow blurred over 16: the light it spreads kept, %.1f of %.1f, as round as its circle, "
                         "half dark at the circle's edge",
                         Sum(*Kept), Sum(*Sharp)));
    }

    /* Spread: a square grown by 4 — its sides, and quarter circles at its
     * corners; shrunk by 4, a square 8 smaller. */
    {
        Shadow Grown, Shrunk;
        Grown.Spread  = 4;
        Shrunk.Spread = -4;
        const Path Square = Path::Rect({50, 50, 60, 40});
        const double Big = Sum(*Grown.MaskFor(Square)), Small = Sum(*Shrunk.MaskFor(Square));
        const double BigWant = 60.0 * 40.0 + 2.0 * 4.0 * (60.0 + 40.0) + M_PI * 16.0;
        Count.Check(Near(Big, BigWant, 0.003) && Near(Small, 52.0 * 32.0, 0.003),
                    Said("a square's shadow spread by 4: %.1f, round at the corners; by -4: %.1f, 8 smaller", Big, Small));
    }

    /* Inner: within its shape only; offset down, darkest under the top edge. */
    {
        Shadow Pressed;
        Pressed.Inner  = true;
        Pressed.Offset = {0, 6};
        Pressed.Blur   = 8;
        const std::optional<Mask> Kept = Pressed.MaskFor(Card);
        const std::optional<Mask> Body = Mask::Of(Card, {}, FillRule::NonZero, Rect{0, 0, 300, 300});
        bool Within = true;
        for (std::int32_t Y = 0; Y < 200; Y++)
            for (std::int32_t X = 0; X < 220; X++) Within = Within && Kept->At(X, Y) <= Body->At(X, Y);
        Count.Check(Within && Kept->At(100, 32) > 150 && Kept->At(100, 107) < 40 && Kept->At(100, 70) < 10,
                    Said("an inner shadow lies within its shape only — dark under its top edge (%.0f), light over its "
                         "bottom (%.0f), none in its middle",
                         Kept->At(100, 32), Kept->At(100, 107)));
    }

    /* A canvas blurred: its flat colour kept, its mean kept, an edge
     * softened alike on both sides. */
    {
        std::optional<Canvas> C = Canvas::Make(120, 60);
        C->Clear(Color::Rgb(30, 60, 90));
        C->Fill(Path::Rect({60, 0, 60, 60}), Fill(Color::Rgb(220, 200, 180)));
        double Before = 0;
        for (std::uint32_t Y = 0; Y < 60u; Y++)
            for (std::uint32_t X = 0; X < 120u; X++) Before += C->Row(Y)[X].Red;
        const Pixel Flat = C->Row(30)[5];
        C->Blur(Rect{0, 0, 120, 60}, 10);
        double After = 0;
        for (std::uint32_t Y = 0; Y < 60u; Y++)
            for (std::uint32_t X = 0; X < 120u; X++) After += C->Row(Y)[X].Red;
        bool Rising = true;
        for (std::uint32_t X = 40; X < 80u; X++) Rising = Rising && C->Row(30)[X].Red <= C->Row(30)[X + 1].Red;
        Count.Check(Near(After, Before, 0.002) && C->Row(30)[5] == Flat && Rising && C->Row(30)[55].Red > Flat.Red &&
                        C->Row(30)[64].Red < C->Row(30)[100].Red,
                    Said("a canvas blurred over 10: its mean kept (%.0f of %.0f), its flat colour flat, its edge soft",
                         After, Before));
    }
}

}  // namespace stand
