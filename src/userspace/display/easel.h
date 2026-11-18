#ifndef BOXOS_DISPLAY_EASEL_H
#define BOXOS_DISPLAY_EASEL_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "box/cxx/error.h"
#include "box/items.h"
#include "box/screen.h"
#include "box/touch.h"
#include "boxos_brush.h"
#include "cell_grid.h"
#include "mirror.h"

// The easel — the glass, and the only hand that draws on it (box/screen.h):
// the screen the kernel lends this program.
//
// On a framebuffer the easel holds the glass's mirror (mirror.h): a frame is
// put into it, and what waits in it goes onto the glass (Flush) — as it is,
// or through an effect over the glass (FlushThrough). Video memory is
// written, never read; the Emergency Screen given back, the mirror goes on
// whole as it is, nothing drawn again.
//
// On a text page each cell is written as glyph and attribute where it
// differs from what the page shows, the caret's attribute turned over; a
// marked cell shows the mark's colour behind its glyph.
class Easel {
public:
    // The screen, claimed — after waiting for it to be free if the program
    // that holds it is ending. One a living program holds is not waited for
    // (errc access_denied): it may be held for good.
    static std::expected<Easel, box::error> Claim() noexcept;

    Easel(Easel &&Other) noexcept;
    Easel &operator=(Easel &&) = delete;
    Easel(const Easel &) = delete;
    Easel &operator=(const Easel &) = delete;
    ~Easel();

    // Whether the glass is a framebuffer, not a text page.
    bool Pixels() const noexcept { return Description.kind == SCREEN_KIND_PIXELS; }

    // A text page's cells; a framebuffer's pixels — 0 for the other glass.
    std::uint32_t Cols() const noexcept { return Pixels() ? 0 : Description.width; }
    std::uint32_t Rows() const noexcept { return Pixels() ? 0 : Description.height; }
    std::uint32_t PixelWidth() const noexcept { return Pixels() ? Description.width : 0; }
    std::uint32_t PixelHeight() const noexcept { return Pixels() ? Description.height : 0; }

    // What the loader left on the glass, as the claim says (SCREEN_LEFT_*),
    // and the fid of the scene whose first frame it is (SCREEN_LEFT_SCENE).
    std::uint32_t Left() const noexcept { return Description.left; }
    std::uint64_t LeftScene() const noexcept { return Description.scene; }

    // A framebuffer's mirror, which frames are put into.
    Mirror &Picture() noexcept { return *Kept; }

    // Count rectangles of a canvas as big as a framebuffer into the mirror and
    // onto the glass, pixel for pixel (boxos_brush.h BoxBrushShow).
    void Show(const BoxBrushCanvas &Canvas, const BoxRect *Rects, std::uint32_t Count) noexcept;

    // The screen's turns, said on the `screen` Touch: Ctrl+Alt+Esc showing the
    // kernel's Emergency Screen (nothing goes onto the glass while it does)
    // and giving the glass back (the mirror goes on whole at the next Flush, a
    // text page is written whole at the next Present). True when the glass
    // was given back.
    bool Step() noexcept;

    // Whether the kernel's Emergency Screen has the glass.
    bool Lent() const noexcept { return Away; }

    // What waits in the mirror put onto a framebuffer — as it is, or through
    // Paint, which writes the whole glass from the mirror itself (an effect
    // over the glass): Paint(Mirror, MirrorStride, Width, Height, Glass,
    // GlassStride, Format).
    void Flush() noexcept;
    template <typename Painting>
    void FlushThrough(Painting &&Paint) noexcept
    {
        if (!Kept) return;
        Kept->Forget();
        if (Away) return;
        Paint(Kept->Pixels(), static_cast<std::size_t>(Kept->Width()), Kept->Width(), Kept->Height(), GlassAt(0, 0),
              static_cast<std::size_t>(Description.stride / 4u), Description.format);
        Fence();
    }

    // Rows() x Cols() cells onto a text page, and the caret: each written
    // where it differs from what the page shows; Text and Page the screen's
    // own colours, which COLOR_DEFAULT is written in.
    void Present(std::span<const ItemCell> Cells, Caret At, std::uint32_t Text, std::uint32_t Page) noexcept;

private:
    Easel(const ScreenDescription &Description, TouchTag Turns, std::optional<Mirror> Kept) noexcept;

    std::uint32_t *GlassAt(std::uint32_t X, std::uint32_t Y) const noexcept
    {
        return reinterpret_cast<std::uint32_t *>(static_cast<std::uintptr_t>(Description.va) +
                                                 static_cast<std::size_t>(Y) * Description.stride +
                                                 static_cast<std::size_t>(X) * 4u);
    }
    static void Fence() noexcept;
    void WriteCell(std::uint32_t Col, std::uint32_t Row, const ItemCell &Cell, bool CaretHere, std::uint32_t Text,
                   std::uint32_t Page) noexcept;

    ScreenDescription Description{};
    TouchTag Turns = TOUCH_TAG_INVALID;
    std::optional<Mirror> Kept;    // a framebuffer's
    std::vector<ItemCell> Shown;   // a text page's cells as the page shows them
    bool ShownKnown = false;       // false: what the text page shows is not ours — write it all
    std::uint32_t ShownText = 0, ShownPage = 0;   // the screen's own colours it was written in
    Caret Drawn;                   // where the text page's caret is
    bool Away = false;
    bool Held = false;
};

#endif
