#include "easel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "box/color.h"
#include "box/error.h"
#include "box/turnin.h"
#include "boxos_glyph_fold.h"
#include "boxos_pixel.h"
#include "typesetter.h"

namespace {

bool SameCell(const GlassCell &Left, const GlassCell &Right) noexcept
{
    return Left.Cell.cp == Right.Cell.cp && Left.Cell.fg == Right.Cell.fg && Left.Cell.bg == Right.Cell.bg &&
           Left.Mark == Right.Mark && (Left.Mark == DISPLAY_CELL_MARK_NONE || Left.MarkColor == Right.MarkColor);
}

// A colour as the glass shows it: a triple as it is, COLOR_DEFAULT as the
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
    if (D.kind == SCREEN_KIND_PIXELS) return D.stride >= D.width * 4u;
    return false;
}

// Light, not numbers. An sRGB number is not the light it shows: a pixel half
// covered by ink shows half the ink's light and half the paper's, and half of
// each number would show darker than that — light text on a dark page thin,
// dark text on a light one heavy. So the two colours are mixed as light, 0 to
// 65535 here, and the light written back as the number nearest it, read in
// 4096 steps.
struct LightTables {
    std::uint16_t Light[256];
    std::uint8_t  Number[4096];
};

LightTables MakeLightTables() noexcept
{
    LightTables Made{};
    for (int Code = 0; Code < 256; Code++) {
        const double Value = Code / 255.0;
        const double Light = Value <= 0.04045 ? Value / 12.92 : std::pow((Value + 0.055) / 1.055, 2.4);
        Made.Light[Code]   = static_cast<std::uint16_t>(std::lround(Light * 65535.0));
    }
    for (int Step = 0; Step < 4096; Step++) {
        const double Light = (Step + 0.5) / 4096.0;
        const double Value = Light <= 0.0031308 ? Light * 12.92 : 1.055 * std::pow(Light, 1 / 2.4) - 0.055;
        Made.Number[Step]  = static_cast<std::uint8_t>(std::lround(std::clamp(Value, 0.0, 1.0) * 255.0));
    }
    return Made;
}

const LightTables Tables = MakeLightTables();

// A cell's two colours, ink and paper, as light: the colour of a pixel the
// ink covers Share parts in 255 of.
class Blend {
public:
    Blend(std::uint32_t Ink, std::uint32_t Paper) noexcept
    {
        for (int Channel = 0; Channel < 3; Channel++) {
            InkLight[Channel]   = Tables.Light[(Ink >> (16 - 8 * Channel)) & 0xFFu];
            PaperLight[Channel] = Tables.Light[(Paper >> (16 - 8 * Channel)) & 0xFFu];
        }
    }

    std::uint32_t At(std::uint8_t Share) const noexcept
    {
        std::uint32_t Colour = 0;
        for (int Channel = 0; Channel < 3; Channel++) {
            const std::int32_t Light = PaperLight[Channel] + (InkLight[Channel] - PaperLight[Channel]) * Share / 255;
            Colour = Colour << 8 | Tables.Number[Light >> 4];
        }
        return Colour;
    }

private:
    std::int32_t InkLight[3];
    std::int32_t PaperLight[3];
};

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
        Shown.resize(static_cast<std::size_t>(ColsValue) * RowsValue);
    }
}

Easel::Easel(Easel &&Other) noexcept
    : Description(Other.Description), Type(Other.Type), Turns(Other.Turns), ColsValue(Other.ColsValue),
      RowsValue(Other.RowsValue), Shown(std::move(Other.Shown)), TextColour(Other.TextColour),
      PageColour(Other.PageColour), ShownKnown(Other.ShownKnown), Drawn(Other.Drawn), Away(Other.Away),
      Held(Other.Held)
{
    Other.Held = false;
    Other.Turns = TOUCH_TAG_INVALID;
}

Easel::~Easel()
{
    if (Held) (void)screen_release();
    if (Turns != TOUCH_TAG_INVALID) touch_release(Turns);
}

void Easel::SetTypesetter(Typesetter &With)
{
    if (!Pixels()) return;
    const std::uint32_t Cols = std::max<std::uint32_t>(1, Description.width / static_cast<std::uint32_t>(With.Cell().Width));
    const std::uint32_t Rows = std::max<std::uint32_t>(1, Description.height / static_cast<std::uint32_t>(With.Cell().Height));
    Shown.assign(static_cast<std::size_t>(Cols) * Rows, GlassCell{});
    Type = &With;
    ColsValue = Cols;
    RowsValue = Rows;
    OriginX = (Description.width - std::min(Description.width, Cols * static_cast<std::uint32_t>(With.Cell().Width))) / 2;
    OriginY = (Description.height - std::min(Description.height, Rows * static_cast<std::uint32_t>(With.Cell().Height))) / 2;
    ShownKnown = false;
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

void Easel::Show(const BoxBrushCanvas &Canvas, const BoxBrushRect *Rects, std::uint32_t Count) noexcept
{
    if (Away || !Pixels() || Canvas.Width != Description.width || Canvas.Height != Description.height) return;
    volatile std::uint8_t *Glass =
        reinterpret_cast<volatile std::uint8_t *>(static_cast<std::uintptr_t>(Description.va));
    for (std::uint32_t I = 0; I < Count; I++) BoxBrushShow(&Canvas, Rects[I], Glass, Description.stride, Description.format);
    ShownKnown = false;
}

void Easel::Page(std::uint32_t Text, std::uint32_t PageValue) noexcept
{
    if (Text == TextColour && PageValue == PageColour) return;
    TextColour = Text;
    PageColour = PageValue;
    ShownKnown = false;
}

void Easel::DrawCell(std::uint32_t Col, std::uint32_t Row, const GlassCell &Cell,
                     bool CaretHere) noexcept
{
    volatile std::uint8_t *Base = reinterpret_cast<volatile std::uint8_t *>(
        static_cast<std::uintptr_t>(Description.va));
    const std::uint32_t Ink = Own(Cell.Cell.fg, TextColour);
    const std::uint32_t Paper = Own(Cell.Cell.bg, PageColour);
    const std::uint32_t MarkInk = Own(Cell.MarkColor, TextColour);
    const bool Under = Cell.Mark == DISPLAY_CELL_MARK_UNDER;
    const bool Bar = Cell.Mark == DISPLAY_CELL_MARK_BAR;

    if (Description.kind == SCREEN_KIND_TEXT) {
        // No pixels to draw a mark with: the mark's colour behind the glyph,
        // and the glyph in the paper's when its own would vanish into it.
        const std::uint32_t Behind = Under || Bar ? MarkInk : Paper;
        const std::uint32_t Front =
            (Under || Bar) && BoxColorToVga4(Ink) == BoxColorToVga4(Behind) ? Paper : Ink;
        std::uint8_t Attribute = BoxColorPairToAttr(Front, Behind);
        if (CaretHere)
            Attribute = static_cast<std::uint8_t>((Attribute << 4) | (Attribute >> 4));
        volatile std::uint8_t *At = Base + static_cast<std::size_t>(Row) * Description.stride +
                                    static_cast<std::size_t>(Col) * 2u;
        At[0] = BoxGlyphFoldOne(Cell.Cell.cp);
        At[1] = Attribute;
        return;
    }

    const std::uint32_t Foreground = BoxPixelEncode(Ink, Description.format);
    const std::uint32_t Background = BoxPixelEncode(Paper, Description.format);
    const std::uint32_t Marking = BoxPixelEncode(MarkInk, Description.format);
    const std::uint32_t CaretInk =
        BoxPixelEncode(Ink != Paper ? Ink : COLOR_LIGHT_GRAY, Description.format);
    const Blend Mix(Ink, Paper);

    const typecase::Size &Fit = Type->Cell();
    const typecase::GlyphImage &Picture = Type->Glyph(Cell.Cell.cp);
    const std::uint32_t Left = OriginX + Col * static_cast<std::uint32_t>(Fit.Width);
    const std::uint32_t Top = OriginY + Row * static_cast<std::uint32_t>(Fit.Height);
    const std::int32_t Wide = static_cast<std::int32_t>(std::min<std::uint32_t>(Fit.Width, Description.width - Left));
    const std::int32_t Tall = static_cast<std::int32_t>(std::min<std::uint32_t>(Fit.Height, Description.height - Top));
    // The caret and a line under the glyph: an eighth of the cell at its foot,
    // two rows at least. A bar: a quarter of its width, two pixels at least.
    const std::int32_t Foot = Fit.Height - std::max(2, Fit.Height / 8);
    const std::int32_t BarWide = std::max(2, Fit.Width / 4);
    // Where the picture's first row stands in the cell: it hangs from the pen
    // on the cell's baseline.
    const std::int32_t PictureTop = Fit.Baseline + Picture.Top;

    volatile std::uint8_t *Line = Base + static_cast<std::size_t>(Top) * Description.stride +
                                  static_cast<std::size_t>(Left) * 4u;
    for (std::int32_t Y = 0; Y < Tall; Y++, Line += Description.stride) {
        volatile std::uint32_t *Pixel = reinterpret_cast<volatile std::uint32_t *>(Line);
        if (Y >= Foot && (CaretHere || Under)) {
            const std::uint32_t Across = CaretHere ? CaretInk : Marking;
            for (std::int32_t X = 0; X < Wide; X++) Pixel[X] = Across;
            continue;
        }
        const std::int32_t Into = Y - PictureTop;
        const std::uint8_t *Shares = Into >= 0 && Into < Picture.Height
                                         ? Picture.Coverage.data() + static_cast<std::size_t>(Into) * static_cast<std::size_t>(Picture.Width)
                                         : nullptr;
        for (std::int32_t X = 0; X < Wide; X++) {
            if (Bar && X < BarWide) {
                Pixel[X] = Marking;
                continue;
            }
            const std::int32_t Along = X - Picture.Left;
            const std::uint8_t Share = Shares && Along >= 0 && Along < Picture.Width ? Shares[Along] : 0;
            Pixel[X] = Share == 0     ? Background
                       : Share == 255 ? Foreground
                                      : BoxPixelEncode(Mix.At(Share), Description.format);
        }
    }
}

// The framebuffer's pixels round the cells, in the page colour: what the
// cells do not fill when the glass is no whole number of them.
void Easel::PaintMargin() noexcept
{
    const std::uint32_t Right = std::min(Description.width, OriginX + ColsValue * static_cast<std::uint32_t>(Type->Cell().Width));
    const std::uint32_t Bottom = std::min(Description.height, OriginY + RowsValue * static_cast<std::uint32_t>(Type->Cell().Height));
    const std::uint32_t Page = BoxPixelEncode(PageColour, Description.format);
    volatile std::uint8_t *Line = reinterpret_cast<volatile std::uint8_t *>(static_cast<std::uintptr_t>(Description.va));
    for (std::uint32_t Y = 0; Y < Description.height; Y++, Line += Description.stride) {
        volatile std::uint32_t *Pixel = reinterpret_cast<volatile std::uint32_t *>(Line);
        const auto Paint = [&](std::uint32_t From, std::uint32_t To) {
            for (std::uint32_t X = From; X < To; X++) Pixel[X] = Page;
        };
        if (Y < OriginY || Y >= Bottom) {
            Paint(0, Description.width);
            continue;
        }
        Paint(0, OriginX);
        Paint(Right, Description.width);
    }
}

void Easel::Present(std::span<const GlassCell> Cells, Caret At) noexcept
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

    if (!ShownKnown && Type) PaintMargin();
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
