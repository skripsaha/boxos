/*
 * canvas.cpp — a canvas of the program's own pixels, kept in an area. See
 * painter/gui/items.h.
 */

#include <cstring>
#include <utility>

#include "box/touch.h"
#include "box/turnin.h"
#include "painter/gui/area.h"
#include "painter/gui/items.h"

namespace painter::gui {

namespace detail {

gui::Canvas MakeCanvas(const std::shared_ptr<Anchor> &Where, std::uint32_t Parent, core::Size Pixels)
{
    if (!Where || Where->Describing || !Where->Owner || Pixels.Width <= 0 || Pixels.Height <= 0) return {};
    Area                                   &Kept  = *Where->Owner;
    std::expected<CanvasBlock, box::error> Pages = Kept.OpenCanvas(static_cast<std::uint32_t>(Pixels.Width),
                                                                    static_cast<std::uint32_t>(Pixels.Height));
    if (!Pages) return {};
    const std::uint32_t Block = Pages->Id;
    gui::Canvas         Made(Where, Make(Where, Parent, gui::Kind::Canvas, gui::Role::None), std::move(*Pages));
    Kept.Changes::Canvas(Made.Id, Block);
    return Made;
}

}  // namespace detail

/* ---- Canvas ---------------------------------------------------------------- */

Canvas::Canvas(std::shared_ptr<detail::Anchor> WhereValue, detail::Made Made, CanvasBlock Pages) noexcept
    : Words(std::move(WhereValue), Made), Block(std::move(Pages))
{
}

Canvas::Canvas(Canvas &&Other) noexcept
    : Words(std::move(Other)), Block(std::move(Other.Block)), Drawing(Other.Drawing), Shown(Other.Shown), Last(Other.Last),
      Painted{Other.Painted[0], Other.Painted[1]}
{
    Other.Block = {};
    Other.Shown = -1;
}

Canvas &Canvas::operator=(Canvas &&Other) noexcept
{
    if (this == &Other) return *this;
    Close();
    Words::operator=(std::move(Other));
    Block       = std::move(Other.Block);
    Drawing     = Other.Drawing;
    Shown       = Other.Shown;
    Last        = Other.Last;
    Painted[0]  = Other.Painted[0];
    Painted[1]  = Other.Painted[1];
    Other.Block = {};
    Other.Shown = -1;
    return *this;
}

Canvas::~Canvas() { Close(); }

void Canvas::Close() noexcept
{
    Area *Kept = Owns && Where ? Where->Owner : nullptr;
    const std::uint32_t Pages = Block.Id;
    Remove();
    if (Kept && Pages) Kept->CloseBlock(Pages);
    Block = {};
}

std::optional<core::Canvas> Canvas::Begin(std::optional<core::Rect> Changed)
{
    Area *Kept = Where && !Where->Describing ? Where->Owner : nullptr;
    if (!Kept || !Block.Memory) return std::nullopt;
    /* The page not given last, once the display lets it go — as soon as it
     * holds the one given after it, so what is said, that page with it, goes
     * first. Waiting, what the display tells the area is kept for the
     * program's Wait or Next. */
    const std::uint32_t Page  = Shown < 0 ? 0u : 1u - static_cast<std::uint32_t>(Shown);
    const bool          Heard = Kept->Bell() != TOUCH_TAG_INVALID;
    Kept->Listen(true);
    while (!Block.Free(Page)) {
        (void)Kept->Present();
        const TurnInMark Mark = box_mark();
        Kept->Gather();
        if (Block.Free(Page)) break;
        if (!Where || !Where->Owner) return std::nullopt;
        box_turn_in(Mark);
    }
    if (!Heard) Kept->Listen(false);
    Drawing                         = Page;
    std::optional<core::Canvas> Into = Block.Page(Page);
    if (!Into) return std::nullopt;
    /* Only part of it to be painted: the rest is the picture shown — which
     * this page holds already but for what the page shown last changed, or
     * all of it for a page never drawn in. */
    const core::Rect Whole{0, 0, static_cast<std::int32_t>(Block.Width), static_cast<std::int32_t>(Block.Height)};
    if (Changed && Changed->Intersect(Whole) != Whole && Shown >= 0) {
        const core::Rect   Behind = Painted[Page] ? Last.Intersect(Whole) : Whole;
        const core::Pixel *From   = Block.PageAt(static_cast<std::uint32_t>(Shown));
        core::Pixel       *To     = Block.PageAt(Page);
        for (std::int32_t Y = Behind.Y; Y < Behind.Y + Behind.Height; Y++) {
            const std::size_t Row = static_cast<std::size_t>(Y) * Block.Width + static_cast<std::size_t>(Behind.X);
            std::memcpy(To + Row, From + Row, static_cast<std::size_t>(Behind.Width) * sizeof(core::Pixel));
        }
    }
    return Into;
}

bool Canvas::End(std::optional<core::Rect> Changed)
{
    Area *Kept = Where && !Where->Describing ? Where->Owner : nullptr;
    if (!Kept || !Block.Memory) return false;
    const core::Rect Whole{0, 0, static_cast<std::int32_t>(Block.Width), static_cast<std::int32_t>(Block.Height)};
    const core::Rect Where2 = Changed ? Changed->Intersect(Whole) : Whole;
    Block.Taken(Drawing);
    Kept->BlockChanged(Block.Id, Where2, Drawing);
    Shown            = static_cast<std::int32_t>(Drawing);
    Last             = Where2;
    Painted[Drawing] = true;
    return true;
}

}  // namespace painter::gui
