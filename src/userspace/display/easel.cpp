#include "easel.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

#include "box/color.h"
#include "box/debug.h"
#include "box/error.h"
#include "box/turnin.h"
#include "boxos_glyph_fold.h"
#include "boxos_pixel.h"
#include "measure.h"

using painter::core::Rect;

namespace {

bool SameCell(const ItemCell &Left, const ItemCell &Right) noexcept
{
    return Left.Character == Right.Character && Left.Ink == Right.Ink && Left.Paper == Right.Paper &&
           Left.Mark == Right.Mark && (Left.Mark == ITEM_MARK_NONE || Left.MarkInk == Right.MarkInk);
}

// A colour as the text page shows it: a triple as it is, COLOR_DEFAULT as the
// screen's own colour for its place.
std::uint32_t Own(std::uint32_t Colour, std::uint32_t ScreenOwn) noexcept
{
    return (Colour >> 24) ? ScreenOwn : Colour;
}

// A description this program can draw on: a known kind, a stride that holds a
// row, and something to draw.
bool Drawable(const ScreenDescription &D) noexcept
{
    if (D.va == 0 || D.width == 0 || D.height == 0) return false;
    if (D.kind == SCREEN_KIND_TEXT) return D.stride >= D.width * 2u;
    if (D.kind == SCREEN_KIND_PIXELS) return D.stride >= D.width * 4u && D.stride % 4u == 0;
    return false;
}

}  // namespace

std::expected<Easel, box::error> Easel::Claim() noexcept
{
    const TouchTag Turns = touch_pair_choose(touch_intern(SCREEN_TOUCH_TAG));
    if (Turns == TOUCH_TAG_INVALID || touch_claim(Turns, TOUCH_REST, 0, 0) != 0)
        return std::unexpected(box::error{box::errc::not_initialized});

    for (;;) {
        ScreenDescription Description{};
        const int Code = screen_claim(&Description);
        if (Code == 0) {
            if (!Drawable(Description)) {
                (void)screen_release();
                return std::unexpected(box::error{box::errc::unsupported});
            }
            // The mirror: a framebuffer's every pixel in this program's own
            // memory, made once.
            std::optional<Mirror> Kept;
            if (Description.kind == SCREEN_KIND_PIXELS) {
                try {
                    Kept.emplace(Description.width, Description.height);
                } catch (const std::bad_alloc &) {
                    /* The claim was made; it is the mirror there is no room
                     * for — said apart from a claim refused. */
                    kdbg_print("[display] no room for the mirror of the glass, %ux%u: the screen is given back",
                               Description.width, Description.height);
                    (void)screen_release();
                    return std::unexpected(box::error{box::errc::no_memory});
                }
            }
            return Easel(Description, Turns, std::move(Kept));
        }
        if (box_errno_of(Code) != ERR_BUSY)
            return std::unexpected(box::error{box_errno_of(Code)});

        // The program that held the screen is ending and its cabin is still
        // being taken down; the kernel says FREE once it is. The tag was claimed before the first
        // ask, so a FREE said after it is banked here and not missed.
        const TurnInMark Mark = box_mark();
        bool Free = false;
        Touch Event;
        while (touch_try_pop_tag(Turns, &Event)) {
            std::uint32_t Turn = 0;
            if (Event.payload_len >= sizeof(Turn)) std::memcpy(&Turn, Event.payload, sizeof(Turn));
            if (Turn == SCREEN_EVENT_FREE) Free = true;
        }
        if (!Free) box_turn_in(Mark);
    }
}

Easel::Easel(const ScreenDescription &DescriptionValue, TouchTag TurnsValue, std::optional<Mirror> KeptValue) noexcept
    : Description(DescriptionValue), Turns(TurnsValue), Kept(std::move(KeptValue)), Held(true)
{
    // A text page's cells as shown, made once: a frame never allocates.
    try {
        if (!Pixels()) Shown.resize(static_cast<std::size_t>(Description.width) * Description.height);
    } catch (...) {
    }
}

Easel::Easel(Easel &&Other) noexcept
    : Description(Other.Description), Turns(Other.Turns), Kept(std::move(Other.Kept)), Shown(std::move(Other.Shown)), ShownKnown(Other.ShownKnown), ShownText(Other.ShownText), ShownPage(Other.ShownPage),
      Drawn(Other.Drawn), Away(Other.Away), Held(Other.Held)
{
    Other.Held = false;
    Other.Turns = TOUCH_TAG_INVALID;
}

Easel::~Easel()
{
    if (Held) (void)screen_release();
    if (Turns != TOUCH_TAG_INVALID) touch_release(Turns);
}

bool Easel::Step() noexcept
{
    bool Back = false;
    Touch Event;
    while (touch_try_pop_tag(Turns, &Event)) {
        std::uint32_t Turn = 0;
        if (Event.payload_len >= sizeof(Turn)) std::memcpy(&Turn, Event.payload, sizeof(Turn));
        if (Turn == SCREEN_EVENT_AWAY) {
            Away = true;
        } else if (Turn == SCREEN_EVENT_BACK) {
            // The Emergency Screen drew over everything: what the glass shows
            // is ours no more.
            Away       = false;
            ShownKnown = false;
            if (Kept) Kept->Refresh();
            Back = true;
        }
    }
    return Back;
}

void Easel::Show(const BoxBrushCanvas &Canvas, const BoxRect *Rects, std::uint32_t Count) noexcept
{
    if (!Kept) return;
    Kept->Show(Canvas, Rects, Count);
    Flush();
}

void Easel::Fence() noexcept
{
    // Write-combining stores wait in the core's buffers until something
    // drains them; the frame is meant to be seen now.
    __asm__ volatile("sfence" ::: "memory");
}

void Easel::Flush() noexcept
{
    if (!Kept) return;
    if (Away) {
        // Nothing goes onto a glass lent away; all of it goes when it is back.
        Kept->Forget();
        return;
    }
    const bool Swapped = Description.format == BOX_PIXEL_RGB;
    Kept->TakeWaiting([&](std::uint32_t Y, std::int32_t From, std::int32_t To) {
        const std::uint32_t *Pixels = Kept->Row(Y) + From;
        std::uint32_t       *Glass  = GlassAt(static_cast<std::uint32_t>(From), Y);
        const auto           Count  = static_cast<std::size_t>(To - From);
        MeasureFlushed(Count);
        if (Swapped)
            for (std::size_t X = 0; X < Count; X++) Glass[X] = BoxPixelEncode(Pixels[X], BOX_PIXEL_RGB);
        else
            std::memcpy(Glass, Pixels, Count * sizeof(std::uint32_t));
    });
    Fence();
}

void Easel::WriteCell(std::uint32_t Col, std::uint32_t Row, const ItemCell &Cell, bool CaretHere, std::uint32_t Text,
                      std::uint32_t Page) noexcept
{
    const std::uint32_t Ink     = Own(Cell.Ink, Text);
    const std::uint32_t Paper   = Own(Cell.Paper, Page);
    const std::uint32_t MarkInk = Own(Cell.MarkInk, Text);
    const bool          Marked  = Cell.Mark == ITEM_MARK_UNDER || Cell.Mark == ITEM_MARK_BAR;
    // No pixels to draw a mark with: the mark's colour behind the glyph, and
    // the glyph in the paper's when its own would vanish into it.
    const std::uint32_t Behind = Marked ? MarkInk : Paper;
    const std::uint32_t Front  = Marked && BoxColorToVga4(Ink) == BoxColorToVga4(Behind) ? Paper : Ink;
    std::uint8_t        Attribute = BoxColorPairToAttr(Front, Behind);
    if (CaretHere) Attribute = static_cast<std::uint8_t>((Attribute << 4) | (Attribute >> 4));
    volatile std::uint8_t *At = reinterpret_cast<volatile std::uint8_t *>(static_cast<std::uintptr_t>(Description.va)) +
                                static_cast<std::size_t>(Row) * Description.stride + static_cast<std::size_t>(Col) * 2u;
    At[0] = BoxGlyphFoldOne(Cell.Character);
    At[1] = Attribute;
}

void Easel::Present(std::span<const ItemCell> Cells, Caret At, std::uint32_t Text, std::uint32_t Page) noexcept
{
    if (Pixels() || Away || Cells.size() != Shown.size()) return;
    const std::uint32_t Cols       = Description.width;
    const bool          CaretMoved = !ShownKnown || At.X != Drawn.X || At.Y != Drawn.Y || At.Visible != Drawn.Visible;
    const std::size_t   None       = Shown.size();
    const std::size_t   OldCaret   = Drawn.Visible && ShownKnown ? static_cast<std::size_t>(Drawn.Y) * Cols + Drawn.X : None;
    const std::size_t   NewCaret   = At.Visible && At.X < Cols && At.Y < Description.height
                                         ? static_cast<std::size_t>(At.Y) * Cols + At.X
                                         : None;
    // A change of the screen's own colours changes every cell drawn in them.
    if (Text != ShownText || Page != ShownPage) ShownKnown = false;
    for (std::size_t Index = 0; Index < Cells.size(); Index++) {
        const bool CaretCell = CaretMoved && (Index == OldCaret || Index == NewCaret);
        if (ShownKnown && !CaretCell && SameCell(Cells[Index], Shown[Index])) continue;
        Shown[Index] = Cells[Index];
        WriteCell(static_cast<std::uint32_t>(Index % Cols), static_cast<std::uint32_t>(Index / Cols), Cells[Index],
                  Index == NewCaret, Text, Page);
    }
    ShownKnown = true;
    ShownText  = Text;
    ShownPage  = Page;
    Drawn      = At;
}
