#ifndef PAINTER_GUI_MEASURE_H
#define PAINTER_GUI_MEASURE_H

// A text measured beforehand, as the display will lay it in the area: on a
// framebuffer laid out in the program's fonts by the very code the display
// lays it with (painter/items/text.h) — its role's style under Style, at most
// Width points wide, wrapped as a text item wraps; on a text page a character
// a cell, a line a row. In points.

#include <string_view>

#include "painter/core/geometry.h"
#include "painter/gui/area.h"
#include "painter/gui/changes.h"

namespace painter::gui {

core::PointF Measure(const Area &On, std::string_view Utf8, gui::Role Of = gui::Role::Body, float Width = ByContent,
                     const TextStyle &Style = {});

namespace detail {
// The same, on the glass On: what Measure does for an area, for painter's own
// stand, which has none.
core::PointF MeasureOn(gui::Glass On, std::string_view Utf8, gui::Role Of, float Width, const TextStyle &Style);
}

}  // namespace painter::gui

#endif
