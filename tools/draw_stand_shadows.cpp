/*
 * draw stand — shadows and blur (painter/core/shadow.h, Mask::Blur,
 * Canvas::Blur): a sharp shadow its shape moved; a blur keeping the light it
 * spreads and its symmetry; spread growing and shrinking a shape by its
 * width; an inner shadow only within its shape, darker toward the light; a
 * canvas blurred keeping its mean and its flat colours; the blur's quick
 * passes giving the blur as defined to the last bit; a kept shadow laid
 * moved by whole pixels as its mask moved.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

#include "draw_stand.h"

#include "blur.h"
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

/* The blur as it is defined, worked the plain way it was first written: every
 * line through each box, a running sum divided by the box's width to the
 * nearest — what the engine's quicker passes must give to the last bit. */
void BoxLineDefined(const std::uint32_t *First, std::size_t Step, std::int32_t Count, std::int32_t Radius, bool Held,
                    std::uint32_t *Out)
{
    const std::int32_t Wide = 2 * Radius + 1;
    const auto         At   = [&](std::int32_t I) -> std::uint64_t {
        if (I < 0 || I >= Count) {
            if (!Held) return 0;
            I = std::clamp(I, 0, Count - 1);
        }
        return First[static_cast<std::size_t>(I) * Step];
    };
    std::uint64_t Total = 0;
    for (std::int32_t K = -Radius; K <= Radius; K++) Total += At(K);
    for (std::int32_t I = 0; I < Count; I++) {
        Out[I] = static_cast<std::uint32_t>((Total + static_cast<std::uint64_t>(Wide / 2)) / static_cast<std::uint64_t>(Wide));
        Total += At(I + Radius + 1);
        Total -= At(I - Radius);
    }
}

void BlurDefined(std::vector<std::uint32_t> &Plane, std::int32_t Width, std::int32_t Height, std::int32_t Channels,
                 const engine::BlurBoxes &Boxes, bool Held)
{
    std::vector<std::uint32_t> Line(static_cast<std::size_t>(std::max(Width, Height)));
    const std::size_t          Across = static_cast<std::size_t>(Channels), Down = static_cast<std::size_t>(Width) * Across;
    for (int Pass = 0; Pass < 3; Pass++) {
        if (Boxes.Radius[Pass] == 0) continue;
        for (std::int32_t Y = 0; Y < Height; Y++)
            for (std::int32_t C = 0; C < Channels; C++) {
                std::uint32_t *First = Plane.data() + static_cast<std::size_t>(Y) * Down + static_cast<std::size_t>(C);
                BoxLineDefined(First, Across, Width, Boxes.Radius[Pass], Held, Line.data());
                for (std::int32_t X = 0; X < Width; X++) First[static_cast<std::size_t>(X) * Across] = Line[static_cast<std::size_t>(X)];
            }
        for (std::int32_t X = 0; X < Width; X++)
            for (std::int32_t C = 0; C < Channels; C++) {
                std::uint32_t *First = Plane.data() + static_cast<std::size_t>(X) * Across + static_cast<std::size_t>(C);
                BoxLineDefined(First, Down, Height, Boxes.Radius[Pass], Held, Line.data());
                for (std::int32_t Y = 0; Y < Height; Y++) First[static_cast<std::size_t>(Y) * Down] = Line[static_cast<std::size_t>(Y)];
            }
    }
}

}  // namespace

void CheckShadows(Tally &Count)
{
    /* The engine's blur, the very numbers of the blur as defined: masks and
     * pixels, planes narrower and wider than their boxes, every width of box
     * up to the widest. */
    {
        std::uint32_t Planes = 0, Wrong = 0;
        for (std::uint32_t I = 0; I < 600u; I++) {
            const float                   Sigma  = I < 520u ? 0.2f + static_cast<float>(I) * 0.5f : static_cast<float>(Random() % 300u);
            const engine::BlurBoxes       Boxes  = engine::BoxesFor(Sigma);
            const bool                    Pixels = I % 2u == 1u;
            const std::int32_t            Width  = 1 + static_cast<std::int32_t>(Random() % (I % 7u == 0 ? 6u : 90u));
            const std::int32_t            Height = 1 + static_cast<std::int32_t>(Random() % (I % 5u == 0 ? 4u : 70u));
            const std::int32_t            Chan   = Pixels ? 4 : 1;
            std::vector<std::uint32_t>    Plane(static_cast<std::size_t>(Width * Height * Chan));
            for (std::uint32_t &Each : Plane) {
                const std::uint32_t Pick = Random() % 4u;
                const std::uint32_t Most = Pixels ? 65535u : 255u;
                Each = Pick == 0 ? 0u : Pick == 1 ? Most : Random() % (Most + 1u);
            }
            std::vector<std::uint32_t> Want = Plane;
            BlurDefined(Want, Width, Height, Chan, Boxes, Pixels);
            if (Pixels) engine::BlurPixels(Plane.data(), Width, Height, Boxes);
            else engine::BlurPlane(Plane.data(), Width, Height, Boxes);
            Planes++;
            Wrong += Plane != Want ? 1u : 0u;
        }
        char Line[200];
        std::snprintf(Line, sizeof(Line), "the blur's quicker passes give the blur as defined to the last bit: %u planes, %u not alike",
                      Planes, Wrong);
        Count.Check(Wrong == 0, Line);
    }
    /* A kept shadow laid moved by whole pixels: the very pixels of its mask
     * moved before it is laid. */
    {
        Shadow Cast;
        Cast.Blur  = 12;
        Cast.Color = Color::Rgba(10, 20, 40, 150);
        const std::optional<Mask> Kept = Cast.MaskFor(Path::RoundedRect({20, 15, 70, 40}, Corners::All(9)));
        std::optional<Canvas>     A = Canvas::Make(160, 120), B = Canvas::Make(160, 120);
        A->Clear(Color::Rgb(200, 210, 220));
        B->Clear(Color::Rgb(200, 210, 220));
        A->DrawShadow(*Kept, Cast, 0.8f, Point{37, -9});
        B->DrawShadow(Kept->Moved(Point{37, -9}), Cast, 0.8f);
        bool Same = true;
        for (std::uint32_t Y = 0; Y < 120u; Y++)
            Same = Same && std::memcmp(A->Row(Y), B->Row(Y), 160u * sizeof(Pixel)) == 0;
        Count.Check(Same, "a kept shadow laid 37 pixels across and 9 up is its mask moved so far and laid");
    }

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
