/* picture.h — the whale on the glass: its pixels worked out where it is, made
 * into cells two pixels tall, and laid in a frame whose every other cell is
 * transparent, so only the whale's own cells are painted over the screen. */
#ifndef WHALE_PICTURE_H
#define WHALE_PICTURE_H

#include <cstdint>
#include <vector>

#include "painter/core/color.h"
#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

#include "figure.h"
#include "swim.h"

namespace whale {

class Picture {
public:
    explicit Picture(painter::core::Size Cells);

    /* Its colours, and whether it has a dark line round it, drawn on its own
     * edge pixels. */
    void Dress(const Hues &Colours, bool Edged);

    /* The whale and its drops drawn into Overlay, which is as large as the
     * glass; the cells that changed since the last time, to composite. */
    painter::core::Rect Draw(const Swimmer &Whale, painter::tui::Frame &Overlay);

private:
    void Clear(painter::core::Rect Area);
    painter::core::Rect Paint(painter::core::Rect Pixels);
    void Edge(painter::core::Rect Pixels);
    void Lay(painter::core::Rect Area, painter::tui::Frame &Overlay) const;
    void Sprinkle(const Swimmer &Whale, painter::tui::Frame &Overlay);

    painter::core::Size                 Cells;
    std::vector<painter::core::Color>   Dots;
    std::vector<Part>                   Parts;
    Figure                              Body;
    bool                                Outlined = true;
    painter::core::Rect                 Shown;
    painter::core::Rect                 Reached;
};

}  // namespace whale

#endif
