#ifndef PAINTER_CORE_PAINT_H
#define PAINTER_CORE_PAINT_H

/* paint.h — a fill made ready to colour the rows of one draw: where each
 * pixel falls in it worked as the brush works it (boxos_paint.h), its colour
 * in sixteen bits. */

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "boxos_paint.h"
#include "painter/core/fill.h"
#include "painter/core/pixel.h"
#include "painter/core/transform.h"

namespace painter::core::engine {

class Prepared {
public:
    /* F, laid by Place onto the canvas, at Opacity; none when it cannot be
     * drawn — a gradient with no stops, a space stretching past what a paint
     * takes, a picture with none. */
    static std::optional<Prepared> Of(const Fill &F, const Transform &Place, float Opacity);

    bool  Solid() const noexcept { return What == Kind::Solid; }
    Pixel Color() const noexcept { return Plain; }
    /* The fill's colours at pixels X0 to X0 + Count of row Y, at its opacity;
     * true, and only Out[0] written, when the row is all one colour — a
     * gradient that runs across the row's way, not along it. */
    bool Row(std::int32_t Y, std::int32_t X0, std::uint32_t Count, Pixel *Out) const;

private:
    enum class Kind : std::uint8_t { Solid, Gradient, Picture };

    bool GradientRow(std::int32_t Y, std::int32_t X0, std::uint32_t Count, Pixel *Out) const;
    Pixel Along(std::uint32_t At) const noexcept;
    void PictureRow(std::int32_t Y, std::int32_t X0, std::uint32_t Count, Pixel *Out) const;

    Kind                                 What = Kind::Solid;
    Pixel                                Plain{};
    BoxPaint                             Paint{};
    BoxPaintReady                        Ready{};
    std::vector<BoxPaintStop>            Stops;
    std::vector<Pixel>                   Table;   /* a gradient's colour every 16 of its way, and one past */
    std::shared_ptr<const core::Picture> Keep;
    const core::Picture::Level          *Level   = nullptr;
    std::uint32_t                        Opacity = 65535u;
};

}  // namespace painter::core::engine

#endif  // PAINTER_CORE_PAINT_H
