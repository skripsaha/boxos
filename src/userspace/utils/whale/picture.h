/* picture.h — the whale on the glass: its pixels worked out where it is, made
 * into cells two pixels tall, and laid in a frame whose every other cell is
 * transparent, so only the whale's own cells are painted over the screen. A
 * pixel on its edge is as opaque as the share of it the whale covers, so the
 * edge is soft and blends into whatever lies beneath, unless it is set to be
 * crisp. */
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

    /* Its colours, whether it has a dark line round it, drawn on its own edge
     * pixels, and whether its edge is soft; if not, every pixel is whole or
     * none. */
    void Dress(const Hues &Colours, bool Edged, bool Smooth);

    /* The whale and its drops drawn into Overlay, which is as large as the
     * glass; the cells that changed since the last time, to composite. */
    painter::core::Rect Draw(const Swimmer &Whale, painter::tui::Frame &Overlay);

private:
    void Clear(painter::core::Rect Area);
    painter::core::Rect Paint(painter::core::Rect Pixels);
    void Edge(painter::core::Rect Pixels);
    bool OnEdge(std::int32_t X, std::int32_t Y) const;
    void Soft(std::int32_t X, std::int32_t Y);
    painter::core::Rect Soften(painter::core::Rect Pixels);
    void Lay(painter::core::Rect Area, painter::tui::Frame &Overlay) const;
    void Sprinkle(const Swimmer &Whale, painter::tui::Frame &Overlay);

    painter::core::Size                 Cells;
    std::vector<painter::core::Color>   Dots;
    std::vector<Part>                   Parts;
    std::vector<std::uint8_t>           Shares;   /* how opaque, of 255, each pixel the whale has is */
    std::vector<std::size_t>            Rim;
    Figure                              Body;
    bool                                Outlined = true;
    bool                                Softened = false;
    painter::core::Rect                 Shown;
    painter::core::Rect                 Reached;
};

}  // namespace whale

#endif
