#include "easel.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "box/color.h"
#include "box/error.h"
#include "box/turnin.h"
#include "boxos_glyph_fold.h"
#include "boxos_pixel.h"
#include "typesetter.h"

using painter::core::Rect;

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
      RowsValue(Other.RowsValue), OriginX(Other.OriginX), OriginY(Other.OriginY), Shown(std::move(Other.Shown)),
      Colours(std::move(Other.Colours)), TextColour(Other.TextColour),
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
    Colours.assign(static_cast<std::size_t>(With.Cell().Width), 0);
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

void Easel::Show(const BoxBrushCanvas &Canvas, const BoxRect *Rects, std::uint32_t Count) noexcept
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

    const CellLook      Look       = LookOf(Cell, CaretHere);
    const std::uint32_t Foreground = BoxPixelEncode(Look.Ink, Description.format);
    const std::uint32_t Background = BoxPixelEncode(Look.Paper, Description.format);
    const Rect          At         = CellPixels(Col, Row);
    volatile std::uint8_t *Line = Base + static_cast<std::size_t>(At.Y) * Description.stride +
                                  static_cast<std::size_t>(At.X) * 4u;
    for (std::int32_t Y = 0; Y < At.Height; Y++, Line += Description.stride) {
        volatile std::uint32_t *Pixel = reinterpret_cast<volatile std::uint32_t *>(Line);
        RowOf(Look, Y, At.Width, Colours.data());
        for (std::int32_t X = 0; X < At.Width; X++) {
            const std::uint32_t Colour = Colours[static_cast<std::size_t>(X)];
            Pixel[X] = Colour == Look.Paper ? Background
                       : Colour == Look.Ink ? Foreground
                                            : BoxPixelEncode(Colour, Description.format);
        }
    }
}

Easel::CellLook Easel::LookOf(const GlassCell &Cell, bool CaretHere) const
{
    CellLook Look{};
    Look.Ink       = Own(Cell.Cell.fg, TextColour);
    Look.Paper     = Own(Cell.Cell.bg, PageColour);
    Look.MarkInk   = Own(Cell.MarkColor, TextColour);
    Look.CaretInk  = Look.Ink != Look.Paper ? Look.Ink : COLOR_LIGHT_GRAY;
    Look.Under     = Cell.Mark == DISPLAY_CELL_MARK_UNDER;
    Look.Bar       = Cell.Mark == DISPLAY_CELL_MARK_BAR;
    Look.CaretHere = CaretHere;
    Look.Picture   = &Type->Glyph(Cell.Cell.cp);
    return Look;
}

void Easel::RowOf(const CellLook &Look, std::int32_t Y, std::int32_t Wide, std::uint32_t *Out) const noexcept
{
    const typecase::Size       &Fit     = Type->Cell();
    const typecase::GlyphImage &Picture = *Look.Picture;
    // The caret and a line under the glyph: an eighth of the cell at its foot,
    // two rows at least. A bar: a quarter of its width, two pixels at least.
    const std::int32_t Foot    = Fit.Height - std::max(2, Fit.Height / 8);
    const std::int32_t BarWide = std::max(2, Fit.Width / 4);
    if (Y >= Foot && (Look.CaretHere || Look.Under)) {
        std::fill(Out, Out + Wide, Look.CaretHere ? Look.CaretInk : Look.MarkInk);
        return;
    }
    // Where the picture's first row stands in the cell: it hangs from the pen
    // on the cell's baseline.
    const std::int32_t  Into   = Y - (Fit.Baseline + Picture.Top);
    const std::uint8_t *Shares = Into >= 0 && Into < Picture.Height
                                     ? Picture.Coverage.data() + static_cast<std::size_t>(Into) * static_cast<std::size_t>(Picture.Width)
                                     : nullptr;
    for (std::int32_t X = 0; X < Wide; X++) {
        if (Look.Bar && X < BarWide) {
            Out[X] = Look.MarkInk;
            continue;
        }
        const std::int32_t Along = X - Picture.Left;
        const std::uint8_t Share = Shares && Along >= 0 && Along < Picture.Width ? Shares[Along] : 0;
        Out[X] = Share == 0 ? Look.Paper : Share == 255 ? Look.Ink : BoxColorOver(Look.Paper, Look.Ink, Share);
    }
}

// A framebuffer cell's pixels, as far as the glass goes.
Rect Easel::CellPixels(std::uint32_t Col, std::uint32_t Row) const noexcept
{
    const typecase::Size &Fit  = Type->Cell();
    const std::uint32_t   Left = OriginX + Col * static_cast<std::uint32_t>(Fit.Width);
    const std::uint32_t   Top  = OriginY + Row * static_cast<std::uint32_t>(Fit.Height);
    return Rect{static_cast<std::int32_t>(Left), static_cast<std::int32_t>(Top),
                static_cast<std::int32_t>(std::min<std::uint32_t>(Fit.Width, Description.width - Left)),
                static_cast<std::int32_t>(std::min<std::uint32_t>(Fit.Height, Description.height - Top))};
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

void Easel::Present(std::span<const GlassCell> Cells, Caret At, const Over &Above) noexcept
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
    const bool Layered = Type && Above.Changed && !Above.Covered.empty();

    if (!ShownKnown && Type) {
        PaintMargin();
        if (Layered)
            Above.Changed(Rect{0, 0, static_cast<std::int32_t>(Description.width), static_cast<std::int32_t>(Description.height)});
    }
    // The cells beneath what lies over them told as runs along their rows.
    Rect Run{};
    for (std::size_t Index = 0; Index < Cells.size(); Index++) {
        const bool CaretCell = CaretMoved && (Index == OldCaret || Index == NewCaret);
        if (ShownKnown && !CaretCell && SameCell(Cells[Index], Shown[Index])) continue;
        const auto Col = static_cast<std::uint32_t>(Index % ColsValue), Row = static_cast<std::uint32_t>(Index / ColsValue);
        Shown[Index] = Cells[Index];
        if (Layered) {
            const Rect Pixels = CellPixels(Col, Row);
            const bool Beneath = std::any_of(Above.Covered.begin(), Above.Covered.end(),
                                             [&](const Rect &Each) { return !Each.Intersect(Pixels).Empty(); });
            if (Beneath) {
                if (!Run.Empty() && Run.Y == Pixels.Y && Run.X + Run.Width == Pixels.X) Run.Width += Pixels.Width;
                else {
                    if (!Run.Empty()) Above.Changed(Run);
                    Run = Pixels;
                }
                continue;
            }
        }
        DrawCell(Col, Row, Cells[Index], Index == NewCaret);
    }
    if (!Run.Empty()) Above.Changed(Run);

    ShownKnown = true;
    Drawn = At;
    Flush();
}

Rect Easel::Grid() const noexcept
{
    if (!Type) return {};
    return Rect{static_cast<std::int32_t>(OriginX), static_cast<std::int32_t>(OriginY),
                static_cast<std::int32_t>(ColsValue * static_cast<std::uint32_t>(Type->Cell().Width)),
                static_cast<std::int32_t>(RowsValue * static_cast<std::uint32_t>(Type->Cell().Height))}
        .Intersect(Rect{0, 0, static_cast<std::int32_t>(Description.width), static_cast<std::int32_t>(Description.height)});
}

std::int32_t Easel::CellHeight() const noexcept { return Type ? Type->Cell().Height : 0; }

void Easel::Beneath(painter::core::Canvas &Into, painter::core::Point Corner, Rect Area) noexcept
{
    if (!Type) return;
    Area = Area.Intersect(Rect{0, 0, static_cast<std::int32_t>(Description.width), static_cast<std::int32_t>(Description.height)});
    const auto Moved = [&](Rect Of) { return Rect{Of.X - Corner.X, Of.Y - Corner.Y, Of.Width, Of.Height}; };
    const auto Light = [](std::uint32_t Triple) {
        return painter::core::Pixel::Of(painter::core::Color::Rgb(static_cast<std::uint8_t>(Triple >> 16),
                                                                  static_cast<std::uint8_t>(Triple >> 8),
                                                                  static_cast<std::uint8_t>(Triple)));
    };
    /* The page round the cells: the four sides of Area the grid leaves. */
    const Rect Cells = Grid();
    const painter::core::Color Page = painter::core::Color::Rgb(static_cast<std::uint8_t>(PageColour >> 16),
                                                                static_cast<std::uint8_t>(PageColour >> 8),
                                                                static_cast<std::uint8_t>(PageColour));
    const std::int32_t Right = Area.X + Area.Width, Bottom = Area.Y + Area.Height;
    const std::int32_t Top   = std::clamp(Cells.Y, Area.Y, Bottom), Foot = std::clamp(Cells.Y + Cells.Height, Area.Y, Bottom);
    const std::int32_t Left  = std::clamp(Cells.X, Area.X, Right), Side = std::clamp(Cells.X + Cells.Width, Area.X, Right);
    for (const Rect &Margin : {Rect{Area.X, Area.Y, Area.Width, Top - Area.Y}, Rect{Area.X, Foot, Area.Width, Bottom - Foot},
                               Rect{Area.X, Top, Left - Area.X, Foot - Top}, Rect{Side, Top, Right - Side, Foot - Top}})
        if (!Margin.Empty()) Into.Clear(Moved(Margin), Page);
    const Rect Inside = Cells.Intersect(Area);
    if (Inside.Empty()) return;
    const typecase::Size &Fit = Type->Cell();
    const auto FirstCol = static_cast<std::uint32_t>((Inside.X - Cells.X) / Fit.Width);
    const auto LastCol  = static_cast<std::uint32_t>((Inside.X + Inside.Width - 1 - Cells.X) / Fit.Width);
    const auto FirstRow = static_cast<std::uint32_t>((Inside.Y - Cells.Y) / Fit.Height);
    const auto LastRow  = static_cast<std::uint32_t>((Inside.Y + Inside.Height - 1 - Cells.Y) / Fit.Height);
    for (std::uint32_t Row = FirstRow; Row <= LastRow && Row < RowsValue; Row++)
        for (std::uint32_t Col = FirstCol; Col <= LastCol && Col < ColsValue; Col++) {
            const Rect Whole = CellPixels(Col, Row), Part = Whole.Intersect(Inside);
            if (Part.Empty()) continue;
            const bool     CaretHere = Drawn.Visible && Drawn.X == Col && Drawn.Y == Row;
            const CellLook Look      = LookOf(Shown[static_cast<std::size_t>(Row) * ColsValue + Col], CaretHere);
            const painter::core::Pixel Paper = Light(Look.Paper), Ink = Light(Look.Ink);
            for (std::int32_t Y = Part.Y; Y < Part.Y + Part.Height; Y++) {
                RowOf(Look, Y - Whole.Y, Whole.Width, Colours.data());
                painter::core::Pixel *Line = Into.Row(static_cast<std::uint32_t>(Y - Corner.Y)) + (Part.X - Corner.X);
                for (std::int32_t X = 0; X < Part.Width; X++) {
                    const std::uint32_t Colour = Colours[static_cast<std::size_t>(Part.X - Whole.X + X)];
                    Line[X] = Colour == Look.Paper ? Paper : Colour == Look.Ink ? Ink : Light(Colour);
                }
            }
        }
}

void Easel::Put(const painter::core::Canvas &Picture, Rect Area) noexcept
{
    if (Away || !Pixels()) return;
    Area = Area.Intersect(Rect{0, 0, static_cast<std::int32_t>(Description.width), static_cast<std::int32_t>(Description.height)});
    if (Area.Empty()) return;
    auto *Glass = reinterpret_cast<std::uint32_t *>(static_cast<std::uintptr_t>(Description.va) +
                                                    static_cast<std::size_t>(Area.Y) * Description.stride +
                                                    static_cast<std::size_t>(Area.X) * 4u);
    Picture.CopyOut(Rect{0, 0, Area.Width, Area.Height}, Glass, Description.stride / 4u, Description.format,
                    painter::core::Dither::On, painter::core::Point{Area.X, Area.Y});
}

void Easel::Flush() noexcept
{
    // Write-combining stores wait in the core's buffers until something
    // drains them; the frame is meant to be seen now.
    __asm__ volatile("sfence" ::: "memory");
}
