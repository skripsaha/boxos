#ifndef PAINTER_CORE_SHAPE_ROOM_H
#define PAINTER_CORE_SHAPE_ROOM_H

/* shape_room.h — a path built as a shape of the one coverage
 * (boxos_shape.h, boxos_stroke.h), in room that grows to what it needs. */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "boxos_shape.h"
#include "painter/core/fill.h"
#include "painter/core/path.h"
#include "painter/core/stroke.h"
#include "painter/core/transform.h"

namespace painter::core::engine {

/* A transform as the shapes' matrix: none when it stretches past
 * BOX_SHAPE_STRETCH_MOST, moves past the reach, or is not a number. */
std::optional<BoxMatrix> MatrixOf(const Transform &Place) noexcept;

class ShapeRoom {
public:
    /* The path, laid by Place, as a shape held by Rule; false when it is
     * refused. */
    bool Fill(const Path &Shape, const Transform &Place, FillRule Rule);
    /* The line along the path as a shape — its width Widen times the
     * stroke's; false when it is refused. */
    bool Stroke(const Path &Shape, const core::Stroke &Line, const Transform &Place, float Widen);

    BoxShape &Built() noexcept { return Shape; }

private:
    template <typename Draw> bool Build(const Transform &Place, Draw &&Lay);

    std::vector<BoxShapeEdge> Edges = std::vector<BoxShapeEdge>(256);
    std::vector<std::int32_t> Dashes;
    BoxShape                  Shape{};
};

}  // namespace painter::core::engine

#endif  // PAINTER_CORE_SHAPE_ROOM_H
