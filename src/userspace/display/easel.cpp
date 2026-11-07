#include "easel.h"

#include <cstring>
#include <utility>

#include "box/color.h"
#include "box/error.h"
#include "box/turnin.h"
#include "boxos_font.h"
#include "boxos_pixel.h"

namespace {

bool SameCell(const TextCell &Left, const TextCell &Right) noexcept
{
    return Left.ch == Right.ch && Left.fg == Right.fg && Left.bg == Right.bg;
}

// A description this program can draw on: a known kind, a stride that holds a
// row, and at least one cell.
bool Drawable(const ScreenDescription &D) noexcept
{
    if (D.va == 0) return false;
    if (D.kind == SCREEN_KIND_TEXT)
        return D.width != 0 && D.height != 0 && D.stride >= D.width * 2u;
    if (D.kind == SCREEN_KIND_PIXELS)
        return D.width >= BOXOS_FONT_WIDTH && D.height >= BOXOS_FONT_HEIGHT &&
               D.stride >= D.width * 4u;
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
            return Easel(Description, Turns);
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

Easel::Easel(const ScreenDescription &DescriptionValue, TouchTag TurnsValue) noexcept
    : Description(DescriptionValue), Turns(TurnsValue), Held(true)
{
    if (Description.kind == SCREEN_KIND_TEXT) {
        ColsValue = Description.width;
        RowsValue = Description.height;
    } else {
        ColsValue = Description.width / BOXOS_FONT_WIDTH;
        RowsValue = Description.height / BOXOS_FONT_HEIGHT;
    }
    Shown.resize(static_cast<std::size_t>(ColsValue) * RowsValue);
}

Easel::Easel(Easel &&Other) noexcept
    : Description(Other.Description), Turns(Other.Turns), ColsValue(Other.ColsValue),
      RowsValue(Other.RowsValue), Shown(std::move(Other.Shown)),
      ShownKnown(Other.ShownKnown), Drawn(Other.Drawn), Away(Other.Away), Held(Other.Held)
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
    bool Changed = false;
    Touch Event;
    while (touch_try_pop_tag(Turns, &Event)) {
        std::uint32_t Turn = 0;
        if (Event.payload_len >= sizeof(Turn)) std::memcpy(&Turn, Event.payload, sizeof(Turn));
        if (Turn == SCREEN_EVENT_AWAY) {
            Away = true;
        } else if (Turn == SCREEN_EVENT_BACK) {
            // The Emergency Screen drew over everything; what the glass
            // shows is no longer known to be ours.
            Away = false;
            ShownKnown = false;
            Changed = true;
        }
    }
    return Changed;
}

void Easel::DrawCell(std::uint32_t Col, std::uint32_t Row, const TextCell &Cell,
                     bool CaretHere) noexcept
{
    volatile std::uint8_t *Base = reinterpret_cast<volatile std::uint8_t *>(
        static_cast<std::uintptr_t>(Description.va));

    if (Description.kind == SCREEN_KIND_TEXT) {
        std::uint8_t Attribute = BoxColorPairToAttr(Cell.fg, Cell.bg);
        if (CaretHere)
            Attribute = static_cast<std::uint8_t>((Attribute << 4) | (Attribute >> 4));
        volatile std::uint8_t *At = Base + static_cast<std::size_t>(Row) * Description.stride +
                                    static_cast<std::size_t>(Col) * 2u;
        At[0] = static_cast<std::uint8_t>(Cell.ch);
        At[1] = Attribute;
        return;
    }

    const std::uint32_t Foreground = BoxPixelEncode(Cell.fg, Description.format);
    const std::uint32_t Background = BoxPixelEncode(Cell.bg, Description.format);
    const std::uint32_t CaretInk =
        BoxPixelEncode(Cell.fg != Cell.bg ? Cell.fg : COLOR_LIGHT_GRAY, Description.format);
    const unsigned char *Glyph = BoxFontGlyphs[static_cast<std::uint8_t>(Cell.ch)];
    volatile std::uint8_t *Line =
        Base + static_cast<std::size_t>(Row) * BOXOS_FONT_HEIGHT * Description.stride +
        static_cast<std::size_t>(Col) * BOXOS_FONT_WIDTH * 4u;
    for (std::uint32_t Y = 0; Y < BOXOS_FONT_HEIGHT; Y++, Line += Description.stride) {
        volatile std::uint32_t *Pixel = reinterpret_cast<volatile std::uint32_t *>(Line);
        if (CaretHere && Y >= BOXOS_FONT_HEIGHT - 2u) {
            for (std::uint32_t X = 0; X < BOXOS_FONT_WIDTH; X++) Pixel[X] = CaretInk;
            continue;
        }
        const std::uint8_t Bits = Glyph[Y];
        for (std::uint32_t X = 0; X < BOXOS_FONT_WIDTH; X++)
            Pixel[X] = (Bits & (0x80u >> X)) ? Foreground : Background;
    }
}

void Easel::Present(std::span<const TextCell> Cells, Caret At) noexcept
{
    if (Away || Cells.size() != Shown.size()) return;

    const bool CaretMoved = !ShownKnown || At.X != Drawn.X || At.Y != Drawn.Y ||
                            At.Visible != Drawn.Visible;
    const std::size_t None = Shown.size();
    const std::size_t OldCaret = Drawn.Visible && ShownKnown
                                     ? static_cast<std::size_t>(Drawn.Y) * ColsValue + Drawn.X
                                     : None;
    const std::size_t NewCaret = At.Visible && At.X < ColsValue && At.Y < RowsValue
                                     ? static_cast<std::size_t>(At.Y) * ColsValue + At.X
                                     : None;

    for (std::size_t Index = 0; Index < Cells.size(); Index++) {
        const bool CaretCell = CaretMoved && (Index == OldCaret || Index == NewCaret);
        if (ShownKnown && !CaretCell && SameCell(Cells[Index], Shown[Index])) continue;
        DrawCell(static_cast<std::uint32_t>(Index % ColsValue),
                 static_cast<std::uint32_t>(Index / ColsValue), Cells[Index], Index == NewCaret);
        Shown[Index] = Cells[Index];
    }

    ShownKnown = true;
    Drawn = At;
    // Write-combining stores wait in the core's buffers until something
    // drains them; the frame is meant to be seen now.
    __asm__ volatile("sfence" ::: "memory");
}
