/*
 * paint.cpp — a fill made ready to colour the rows of one draw. See paint.h.
 */

#include "paint.h"

#include <algorithm>
#include <cmath>

#include "lanes.h"
#include "light.h"
#include "paint_rows.h"
#include "shape_room.h"

namespace painter::core::engine {

namespace {

constexpr std::int64_t Way = BOX_PAINT_GRADIENT;

std::uint32_t ShareOf(float Opacity) noexcept
{
    if (!(Opacity > 0.0f)) return 0;
    if (Opacity >= 1.0f) return light::Whole;
    return static_cast<std::uint32_t>(std::lround(Opacity * static_cast<float>(light::Whole)));
}

/* A transform onto the canvas as a paint's space whose own places are Unit
 * of the transform's own pixels each — a gradient's whole way GRADIENT places
 * long, ONE / GRADIENT; a picture's pixel ONE places wide, 1 — the shapes'
 * matrix taking a place for 1 / ONE of a pixel: none when it does not fit. */
std::optional<BoxMatrix> SpaceOf(const Transform &Onto, float Unit) noexcept
{
    Transform Scaled = Onto;
    Scaled.A *= Unit, Scaled.B *= Unit, Scaled.C *= Unit, Scaled.D *= Unit;
    return MatrixOf(Scaled);
}

/* A stop's colour as numbers in 1/256 of a number and an opacity of 65535:
 * the screen's own colour has none of either. */
struct Fine {
    std::int64_t Channel[3];
    std::int64_t Alpha;
};

Fine FineOf(core::Color C) noexcept
{
    if (C.IsScreen()) return Fine{{0, 0, 0}, 0};
    return Fine{{static_cast<std::int64_t>(C.Red()) * 256, static_cast<std::int64_t>(C.Green()) * 256,
                 static_cast<std::int64_t>(C.Blue()) * 256},
                static_cast<std::int64_t>(C.Opacity()) * 257};
}

/* Part of 65536 of the way from A to B, to the nearest, halves away from A. */
std::int64_t Walked(std::int64_t A, std::int64_t B, std::int64_t Part) noexcept
{
    return A + BoxShapeRoundDiv((B - A) * Part, 65536);
}

Pixel PixelOfFine(const Fine &F) noexcept
{
    const std::uint32_t Alpha = static_cast<std::uint32_t>(F.Alpha);
    return Pixel{static_cast<std::uint16_t>(light::Times(light::OfFine(static_cast<std::uint32_t>(F.Channel[0])), Alpha)),
                 static_cast<std::uint16_t>(light::Times(light::OfFine(static_cast<std::uint32_t>(F.Channel[1])), Alpha)),
                 static_cast<std::uint16_t>(light::Times(light::OfFine(static_cast<std::uint32_t>(F.Channel[2])), Alpha)),
                 static_cast<std::uint16_t>(Alpha)};
}

std::uint32_t ExtendOf(core::Extend Past) noexcept
{
    switch (Past) {
    case core::Extend::Repeat: return BOX_PAINT_REPEAT;
    case core::Extend::Reflect: return BOX_PAINT_REFLECT;
    case core::Extend::None: return BOX_PAINT_NONE;
    default: return BOX_PAINT_PAD;
    }
}

}  // namespace

std::optional<Prepared> Prepared::Of(const Fill &F, const Transform &Place, float Opacity)
{
    Prepared Made;
    Made.Opacity = ShareOf(Opacity * F.Seen());
    const auto &What = F.What();

    if (const core::Color *Solid = std::get_if<core::Color>(&What)) {
        Made.Plain = light::Scaled(Pixel::Of(*Solid), Made.Opacity);
        return Made;
    }

    if (const Gradient *G = std::get_if<Gradient>(&What)) {
        const auto &Stops = G->Stops();
        if (Stops.empty()) return std::nullopt;
        /* A gradient of no length or radius is its last colour, as SVG
         * paints it. */
        if (G->Space().Determinant() == 0.0f) {
            Made.Plain = light::Scaled(Pixel::Of(Stops.back().Color), Made.Opacity);
            return Made;
        }
        const std::optional<BoxMatrix> Space = SpaceOf(G->Space().Then(Place), static_cast<float>(BOX_SHAPE_ONE) / static_cast<float>(Way));
        if (!Space) return std::nullopt;
        Made.What = Kind::Gradient;
        for (const GradientStop &S : Stops)
            Made.Stops.push_back(BoxPaintStop{static_cast<std::uint32_t>(std::lround(S.Offset * Way)), 0xFF000000u});
        Made.Paint.Kind  = G->Of() == Gradient::Kind::Linear   ? BOX_PAINT_LINEAR
                           : G->Of() == Gradient::Kind::Radial ? BOX_PAINT_RADIAL
                                                               : BOX_PAINT_CONIC;
        Made.Paint.Stops     = Made.Stops.data();
        Made.Paint.StopCount = static_cast<std::uint32_t>(Made.Stops.size());
        Made.Paint.Space     = *Space;
        Made.Paint.Extend    = ExtendOf(G->Past());
        if (Made.Paint.Kind == BOX_PAINT_RADIAL) {
            /* A focal point on or past the circle drawn just within it. */
            float       FX = G->Focus().X, FY = G->Focus().Y;
            const float Far = std::sqrt(FX * FX + FY * FY);
            if (Far > 0.995f) FX *= 0.995f / Far, FY *= 0.995f / Far;
            Made.Paint.FocalX = static_cast<std::int32_t>(std::lround(FX * Way));
            Made.Paint.FocalY = static_cast<std::int32_t>(std::lround(FY * Way));
        }
        if (!BoxPaintPrepare(&Made.Paint, &Made.Ready)) return std::nullopt;

        /* The colour every 16 of the way: between two stops walked straight
         * in their numbers, in sixteen bits. */
        Made.Table.resize(Way / 16 + 2);
        for (std::size_t I = 0; I < Made.Table.size(); I++) {
            const std::int64_t At = std::min<std::int64_t>(static_cast<std::int64_t>(I) * 16, Way);
            std::size_t        K  = 0;
            while (K < Stops.size() && Made.Stops[K].At < At) K++;
            Fine Mixed;
            if (K == 0) {
                Mixed = FineOf(Stops.front().Color);
            } else if (K == Stops.size()) {
                Mixed = FineOf(Stops.back().Color);
            } else {
                const Fine         A = FineOf(Stops[K - 1].Color), B = FineOf(Stops[K].Color);
                const std::int64_t From = Made.Stops[K - 1].At, To = Made.Stops[K].At;
                const std::int64_t Part = To > From ? (At - From) * 65536 / (To - From) : 65536;
                for (int C = 0; C < 3; C++) Mixed.Channel[C] = Walked(A.Channel[C], B.Channel[C], Part);
                Mixed.Alpha = Walked(A.Alpha, B.Alpha, Part);
            }
            Made.Table[I] = light::Scaled(PixelOfFine(Mixed), Made.Opacity);
        }
        return Made;
    }

    const Fill::PictureFill &Shown = std::get<Fill::PictureFill>(What);
    if (!Shown.Source) return std::nullopt;
    const Transform Onto = Shown.Place.Then(Place);
    /* The copy nearest the size it is drawn at: each level a picture's pixel
     * twice as wide, chosen by how much the picture shrinks along its more
     * shrunk side. */
    const float Across = std::sqrt(Onto.A * Onto.A + Onto.B * Onto.B);
    const float Down   = std::sqrt(Onto.C * Onto.C + Onto.D * Onto.D);
    const float Least  = std::min(Across, Down);
    const auto  Sizes  = Shown.Source->Sizes();
    std::size_t Level  = 0;
    if (!Shown.Sharp && Least > 0.0f)
        while (Level + 1 < Sizes.size() && Least * static_cast<float>(2u << Level) <= 1.0f) Level++;
    const float Wide = static_cast<float>(1u << Level);
    const std::optional<BoxMatrix> Space = SpaceOf(Onto, Wide);
    if (!Space) return std::nullopt;
    Made.What                = Kind::Picture;
    Made.Keep                = Shown.Source;
    Made.Level               = &Sizes[Level];
    Made.Paint.Kind          = BOX_PAINT_PICTURE;
    Made.Paint.PictureWidth  = Made.Level->Width;
    Made.Paint.PictureHeight = Made.Level->Height;
    Made.Paint.PictureStride = Made.Level->Width;
    Made.Paint.Extend        = ExtendOf(Shown.Past);
    Made.Paint.Sharp         = Shown.Sharp ? 1u : 0u;
    Made.Paint.Space         = *Space;
    if (!BoxPaintSpace(&Made.Paint, &Made.Ready)) return std::nullopt;
    return Made;
}

bool Prepared::Row(std::int32_t Y, std::int32_t X0, std::uint32_t Count, Pixel *Out) const
{
    if (What == Kind::Gradient) return GradientRow(Y, X0, Count, Out);
    if (What == Kind::Picture) {
        PictureRow(Y, X0, Count, Out);
        return false;
    }
    Out[0] = Plain;
    return true;
}

bool Prepared::GradientRow(std::int32_t Y, std::int32_t X0, std::uint32_t Count, Pixel *Out) const
{
    BoxPaintReady R = Ready;
    R.Paint         = &Paint;
    BoxPaintWalk Walk;
    BoxPaintWalkBegin(&R, X0, Y, &Walk);
    const lanes::GradientWalk Colours{&Paint, Table.data(), Walk.U, Walk.V, Walk.StepU, Walk.StepV};
    /* A linear gradient whose way does not change along the row: one
     * colour, the first pixel's. */
    if (Paint.Kind == BOX_PAINT_LINEAR && Walk.StepU == 0) {
        lanes::GradientPlain(Out, Colours, Walk.U, Walk.V, 1);
        return true;
    }
    lanes::Chosen().Gradient(Out, Colours, Count);
    return false;
}

void Prepared::PictureRow(std::int32_t Y, std::int32_t X0, std::uint32_t Count, Pixel *Out) const
{
    BoxPaintReady R = Ready;
    R.Paint         = &Paint;
    const Pixel *Texels = Level->Pixels.data();
    BoxPaintWalk Walk;
    BoxPaintWalkBegin(&R, X0, Y, &Walk);
    if (!Paint.Sharp) {
        lanes::Chosen().Picture(Out, lanes::PictureWalk{&Paint, Texels, Walk.U, Walk.V, Walk.StepU, Walk.StepV, Opacity}, Count);
        return;
    }
    for (std::uint32_t I = 0; I < Count; I++, BoxPaintWalkNext(&Walk)) {
        std::int64_t U, V;
        std::size_t  Index;
        BoxPaintWalkAt(&Walk, &U, &V);
        Out[I] = BoxPaintTexel(&Paint, BoxShapePixelOf(U), BoxShapePixelOf(V), &Index) ? Texels[Index] : Pixel{};
        if (Opacity < light::Whole) Out[I] = light::Scaled(Out[I], Opacity);
    }
}

}  // namespace painter::core::engine
