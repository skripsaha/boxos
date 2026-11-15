#ifndef TYPECASE_CELL_INK_H
#define TYPECASE_CELL_INK_H

/* A cell being drawn by geometry, in its own pixels, y down from its top:
 * how much each pixel is inked, the union of the shapes laid on it — a
 * pixel covered by two shapes is covered by the more of the two. */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "typecase/glyph_image.h"
#include "typecase/outline.h"
#include "typecase/rasterizer.h"

namespace typecase {

/* The pixel nearest to Index parts in Parts of Length: where a sub-cell's
 * edge lies, the same for every character drawn to the cell. */
constexpr std::int32_t Edge(std::int32_t Index, std::int32_t Parts, std::int32_t Length) noexcept
{
    return (Length * Index + Parts / 2) / Parts;
}

class CellInk {
public:
    CellInk(std::int32_t Width, std::int32_t Height)
        : Width(Width), Height(Height),
          Coverage(static_cast<std::size_t>(Width) * static_cast<std::size_t>(Height), 0)
    {
    }

    /* The pixels of a rectangle inked whole, as much of it as is in the cell. */
    void Fill(std::int32_t Left, std::int32_t Top, std::int32_t Right, std::int32_t Bottom)
    {
        Left = std::max(Left, 0), Top = std::max(Top, 0);
        Right = std::min(Right, Width), Bottom = std::min(Bottom, Height);
        for (std::int32_t Y = Top; Y < Bottom; Y++)
            for (std::int32_t X = Left; X < Right; X++) At(X, Y) = 255;
    }

    /* A shape given in the cell's pixels, y down, smoothed and laid on. */
    void Smooth(const Outline &Shape, Rasterizer &Pen)
    {
        Outline Turned = Shape;
        for (Point &Each : Turned.Points) Each.Y = -Each.Y;
        const GlyphImage Image = Pen.Draw(Turned, 1, 1, Bounds{0, 0, Width, Height}, 0);
        for (std::int32_t Y = 0; Y < Image.Height; Y++)
            for (std::int32_t X = 0; X < Image.Width; X++) {
                std::uint8_t &Here = At(Image.Left + X, Image.Top + Y);
                Here = std::max(Here, Image.Coverage[static_cast<std::size_t>(Y * Image.Width + X)]);
            }
    }

    std::uint8_t &At(std::int32_t X, std::int32_t Y)
    {
        return Coverage[static_cast<std::size_t>(Y) * static_cast<std::size_t>(Width) + static_cast<std::size_t>(X)];
    }

    /* The cell as a picture against the pen on its baseline. */
    GlyphImage Picture(std::int32_t Baseline) &&
    {
        GlyphImage Image;
        Image.Top      = -Baseline;
        Image.Width    = Width;
        Image.Height   = Height;
        Image.Advance  = static_cast<float>(Width);
        Image.Coverage = std::move(Coverage);
        return Image;
    }

    const std::int32_t Width;
    const std::int32_t Height;

private:
    std::vector<std::uint8_t> Coverage;
};

}  // namespace typecase

#endif  // TYPECASE_CELL_INK_H
