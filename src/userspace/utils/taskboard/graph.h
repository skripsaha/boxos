/* graph.h — load drawn in half-block pixels, two to a cell's height: a graph
 * of columns, each as tall as its load, colored from the person's cool at the
 * floor to hot at the top and its edge blended into what is behind it; and a
 * meter, a bar lit in half cells. */
#ifndef TASKBOARD_GRAPH_H
#define TASKBOARD_GRAPH_H

#include <cstdint>
#include <optional>
#include <vector>

#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

#include "look.h"

namespace taskboard {

// Columns into Area, one a cell wide, the newest at the right; Paper behind,
// with faint lines at a quarter, a half and three quarters when Area is tall
// enough to hold them apart.
void DrawGraph(painter::tui::Frame &Out, painter::core::Rect Area, const std::vector<std::optional<float>> &Columns,
               const Look &Shown, Color Paper);

// Share, 0 to 1, as a bar of Width cells from At, each lit cell in the color
// of the load it stands for; what is not lit, faint dots.
void DrawMeter(painter::tui::Frame &Out, painter::core::Point At, std::int32_t Width, double Share,
               const Look &Shown, Color Paper);

}  // namespace taskboard

#endif
