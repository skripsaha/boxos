/*
 * picture.cpp — a picture to fill with, and its smaller copies. See
 * painter/core/picture.h.
 */

#include "painter/core/picture.h"

#include <new>

#include "boxos_shape.h"
#include "light.h"

namespace painter::core {

namespace {

bool Fits(std::uint32_t Width, std::uint32_t Height) noexcept
{
    return Width > 0 && Height > 0 && Width <= static_cast<std::uint32_t>(BOX_SHAPE_REACH) &&
           Height <= static_cast<std::uint32_t>(BOX_SHAPE_REACH);
}

}  // namespace

Picture::Picture(Level Whole)
{
    Levels.push_back(std::move(Whole));
    MakeSmaller();
}

/* Each copy half the one before, each of its pixels the mean of the two by
 * two it stands for — of their light with their opacity, so a fine pattern
 * keeps its brightness — a last odd row or column standing for itself twice. */
void Picture::MakeSmaller()
{
    while (Levels.back().Width > 1 || Levels.back().Height > 1) {
        const Level &From = Levels.back();
        Level        Half;
        Half.Width  = (From.Width + 1u) / 2u;
        Half.Height = (From.Height + 1u) / 2u;
        Half.Pixels.resize(static_cast<std::size_t>(Half.Width) * Half.Height);
        for (std::uint32_t Y = 0; Y < Half.Height; Y++)
            for (std::uint32_t X = 0; X < Half.Width; X++) {
                const std::uint32_t X0 = 2u * X, Y0 = 2u * Y;
                const std::uint32_t X1 = X0 + 1u < From.Width ? X0 + 1u : X0;
                const std::uint32_t Y1 = Y0 + 1u < From.Height ? Y0 + 1u : Y0;
                const Pixel *Four[4] = {&From.Pixels[static_cast<std::size_t>(Y0) * From.Width + X0],
                                        &From.Pixels[static_cast<std::size_t>(Y0) * From.Width + X1],
                                        &From.Pixels[static_cast<std::size_t>(Y1) * From.Width + X0],
                                        &From.Pixels[static_cast<std::size_t>(Y1) * From.Width + X1]};
                const auto Mean = [&](std::uint16_t Pixel::*Of) {
                    std::uint32_t Sum = 2u;
                    for (const Pixel *P : Four) Sum += P->*Of;
                    return static_cast<std::uint16_t>(Sum >> 2);
                };
                Half.Pixels[static_cast<std::size_t>(Y) * Half.Width + X] =
                    Pixel{Mean(&Pixel::Red), Mean(&Pixel::Green), Mean(&Pixel::Blue), Mean(&Pixel::Alpha)};
            }
        Levels.push_back(std::move(Half));
    }
}

std::shared_ptr<const Picture> Picture::FromRgba(std::span<const std::uint8_t> Bytes, std::uint32_t Width,
                                                 std::uint32_t Height, std::size_t Stride)
{
    if (!Fits(Width, Height) || Stride < static_cast<std::size_t>(Width) * 4u ||
        Bytes.size() < Stride * (Height - 1u) + static_cast<std::size_t>(Width) * 4u)
        return nullptr;
    Level Whole;
    Whole.Width  = Width;
    Whole.Height = Height;
    Whole.Pixels.resize(static_cast<std::size_t>(Width) * Height);
    for (std::uint32_t Y = 0; Y < Height; Y++)
        for (std::uint32_t X = 0; X < Width; X++) {
            const std::uint8_t *P = &Bytes[Y * Stride + static_cast<std::size_t>(X) * 4u];
            Whole.Pixels[static_cast<std::size_t>(Y) * Width + X] =
                light::OfArgb(static_cast<std::uint32_t>(P[3]) << 24 | static_cast<std::uint32_t>(P[0]) << 16 |
                              static_cast<std::uint32_t>(P[1]) << 8 | P[2]);
        }
    return std::shared_ptr<const Picture>(new Picture(std::move(Whole)));
}

std::shared_ptr<const Picture> Picture::FromPixels(std::span<const Pixel> Pixels, std::uint32_t Width,
                                                   std::uint32_t Height, std::size_t Stride)
{
    if (!Fits(Width, Height) || Stride < Width || Pixels.size() < Stride * (Height - 1u) + Width) return nullptr;
    Level Whole;
    Whole.Width  = Width;
    Whole.Height = Height;
    Whole.Pixels.resize(static_cast<std::size_t>(Width) * Height);
    for (std::uint32_t Y = 0; Y < Height; Y++)
        for (std::uint32_t X = 0; X < Width; X++)
            Whole.Pixels[static_cast<std::size_t>(Y) * Width + X] = Pixels[Y * Stride + X];
    return std::shared_ptr<const Picture>(new Picture(std::move(Whole)));
}

}  // namespace painter::core
