#ifndef PAINTER_GUI_AREA_H
#define PAINTER_GUI_AREA_H

// A program's area on the screen, and what it shows there: items it makes and
// changes (painter/gui/changes.h), shown a step at a time — Present() puts what
// changed in the area's ring and tells the display once — and what the display
// tells back (Event): where the area is and how large, an item's size by its
// content, a change refused, a step shown and what it cost.
//
// Where the area lies and how large it is are the display's to choose; a
// program says only what it wishes (Placement, a size) and learns what it was
// given. Pictures, canvases of the program's own pixels and fonts go in blocks
// of their own, made by the display for the program, and are named by number
// from the items that show them.
//
// One strand speaks for an area: the one that opened it.

#include <cstdint>
#include <expected>
#include <optional>
#include <span>

#include "box/cxx/bay.h"
#include "box/cxx/error.h"
#include "box/items.h"
#include "box/touch.h"
#include "painter/core/canvas.h"
#include "painter/gui/changes.h"

namespace painter::gui {

enum class Placement : std::uint8_t {
    Over  = DISPLAY_AREA_OVER,    // over everything, where the display puts it
    Whole = DISPLAY_AREA_WHOLE,   // the whole screen, while the program is in front
    Flow  = DISPLAY_AREA_FLOW,    // in the console's flow
};

enum class Glass : std::uint8_t { Text = ITEM_GLASS_TEXT, Pixels = ITEM_GLASS_PIXELS };

struct Event {
    enum class Kind : std::uint8_t { Area, Size, Refused, Shown, PageFree };
    Kind          What  = Kind::Area;
    std::uint32_t Item  = 0;        // the item, or the block
    core::PointF  Size{};           // the area's or the item's, in points
    float         Scale = 1;        // pixels to a point
    gui::Glass    On    = gui::Glass::Pixels;
    bool          Seen  = true;     // the area is shown, not behind
    std::uint16_t Op = 0, Property = 0;
    error_t       Reason = 0;
    std::uint64_t Step   = 0;
    std::uint32_t Micros = 0, Pixels = 0, Bytes = 0, Records = 0;
    std::uint32_t Page   = 0;
};

// A picture: Width x Height pixels of R, G, B, A bytes, the opacity not
// multiplied in. Filled, then shown by the items that name it; changed, and
// said so (Changes::BlockChanged).
struct PictureBlock {
    std::uint32_t          Id = 0;
    std::uint32_t          Width = 0, Height = 0;
    box::bay<std::byte>    Memory;
    std::span<std::uint8_t> Pixels() noexcept;
};

// A canvas of the program's own pixels: two pages, one drawn while the other
// is shown — a page is drawn in only while it is free (Free), and given to the
// display with Changes::BlockChanged naming it.
struct CanvasBlock {
    std::uint32_t       Id = 0;
    std::uint32_t       Width = 0, Height = 0;
    box::bay<std::byte> Memory;
    bool                         Free(std::uint32_t Page) const noexcept;
    std::optional<core::Canvas> Page(std::uint32_t Page) noexcept;   // to draw on, while free
    void                         Taken(std::uint32_t Page) noexcept;  // given to the display
};

class Area : public Changes {
public:
    static std::expected<Area, box::error> Open(Placement Where, core::PointF Wish = {});

    Area(Area &&Other) noexcept;
    Area &operator=(Area &&) = delete;
    Area(const Area &)            = delete;
    Area &operator=(const Area &) = delete;
    ~Area();

    // What changed since the last step put in the ring, and the display told:
    // true when all of it went; false when the ring was full — the rest waits,
    // and goes with the next Present.
    bool Present();

    // What the display told, oldest first; none when nothing waits.
    std::optional<Event> Next();
    // The tag the display touches when it tells something: wait on it.
    TouchTag Bell() const noexcept { return Rung; }

    // The number of the last step put in the ring: the step a Shown event
    // names once the glass shows it.
    std::uint64_t Step() const noexcept { return Steps; }

    // The area's size in points and its pixels to a point — and, as for every
    // Changes, an item's size and scale said.
    using Changes::Scale;
    using Changes::Size;
    core::PointF Size() const noexcept { return Given; }
    float        Scale() const noexcept { return PixelsPerPoint; }
    gui::Glass   On() const noexcept { return Kind; }
    std::uint32_t Id() const noexcept { return AreaId; }

    std::expected<PictureBlock, box::error>  OpenPicture(std::uint32_t Width, std::uint32_t Height);
    std::expected<CanvasBlock, box::error>   OpenCanvas(std::uint32_t Width, std::uint32_t Height);
    std::expected<std::uint32_t, box::error> OpenFont(std::span<const std::uint8_t> Bytes);
    void                                     CloseBlock(std::uint32_t Block);

private:
    Area() = default;
    std::expected<box::bay<std::byte>, box::error> Block(std::uint8_t Kind, std::uint32_t Width, std::uint32_t Height,
                                                         std::uint32_t Bytes, std::uint32_t &Id);
    void Tell(std::uint8_t Command, std::uint32_t What) noexcept;

    std::uint32_t       AreaId     = 0;
    std::uint32_t       DisplayPid = 0;
    box::bay<std::byte> Memory;
    ItemAreaHeader     *Head  = nullptr;
    core::PointF        Given{};
    float               PixelsPerPoint = 1;
    gui::Glass          Kind  = gui::Glass::Pixels;
    TouchTag            Rung  = TOUCH_TAG_INVALID;
    std::uint64_t       Steps = 0;
    bool                Opened = false;
};

}  // namespace painter::gui

#endif  // PAINTER_GUI_AREA_H
