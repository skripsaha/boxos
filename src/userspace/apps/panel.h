#ifndef APPS_PANEL_H
#define APPS_PANEL_H

/* The panel in the screen's top right, which the clock and pulse share
 * (clock.cpp, pulse.cpp): each a notice of its own — an area over everything,
 * as large as the screen and shown always, seen only where its notice lies —
 * the clock at the corner, pulse left of all the clock can ever be. In
 * points; the words in the code face, so a figure never shifts the rest. */

#include <expected>

#include "painter/gui/area.h"
#include "painter/gui/measure.h"

namespace panel {

inline constexpr float              Margin  = 6;   // from the screen's top and right
inline constexpr float              Padding = 8;   // inside a notice, across
inline constexpr float              Inset   = 3;   // inside a notice, down
inline constexpr float              Gap     = 8;   // between the notices
inline constexpr float              Round   = 6;   // a notice's corners
inline constexpr painter::gui::Role Words   = painter::gui::Role::Code;

/* A notice's area: over everything, shown always, wished as large as the
 * language lets a size be — and so as large as the screen. */
inline std::expected<painter::gui::Area, box::error> Open()
{
    constexpr float Largest = static_cast<float>(ITEM_REACH - 1) / ITEM_POINT;
    return painter::gui::Area::Open(painter::gui::Placement::Over, painter::core::PointF{Largest, Largest}, true);
}

/* How tall a notice is: a line of its words, inset. */
inline float Height(const painter::gui::Area &On) { return painter::gui::Measure(On, "0", Words).Y + 2 * Inset; }

/* The clock's notice at its widest — 12:59:59 PM, however the person sets it. */
inline float ClockWidth(const painter::gui::Area &On) { return painter::gui::Measure(On, "12:59:59 PM", Words).X + 2 * Padding; }

}  // namespace panel

#endif
