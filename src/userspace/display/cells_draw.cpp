/*
 * cells_draw.cpp — how a cell looks, on pixels and on a page. See
 * cells_draw.h.
 */

#include "cells_draw.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "painter/core/pixel.h"
#include "typesetter.h"

namespace {

using painter::core::Pixel;
using painter::core::Rect;

constexpr std::uint32_t Whole = 65535u;

/* A cell's colour as it is drawn: its triple and how much of it covers what
 * lies beneath, of 255 — a colour no program chose the screen's own Own. */
struct Ink {
    std::uint32_t Rgb   = 0;
    std::uint32_t Alpha = 255;
};

Ink InkOf(std::uint32_t Colour, std::uint32_t Own) noexcept
{
    if (Colour == COLOR_THROUGH) return Ink{0, 0};
    if (BoxColorIsTranslucent(Colour))
        return Ink{Colour & 0x00FFFFFFu, (BoxColorOpacity(Colour) * 255u + COLOR_OPACITY_STEPS / 2u) / COLOR_OPACITY_STEPS};
    return Ink{(Colour >> 24) ? Own : Colour, 255};
}

Pixel PixelOf(Ink Of) noexcept
{
    return Pixel::Of(painter::core::Color::Rgba(static_cast<std::uint8_t>(Of.Rgb >> 16), static_cast<std::uint8_t>(Of.Rgb >> 8),
                                                static_cast<std::uint8_t>(Of.Rgb), static_cast<std::uint8_t>(Of.Alpha)));
}

/* A cell's colours and picture, worked out once for its rows. */
struct Look {
    Ink                         Paper, Glyph, Mark, Caret;
    bool                        Under = false, Bar = false, CaretHere = false;
    const typecase::GlyphImage *Picture = nullptr;
};

Look LookOf(const ItemCell &Cell, const CellsLook &Screen, bool CaretHere)
{
    Look Made;
    Made.Paper     = InkOf(Cell.Paper, Screen.Page);
    Made.Glyph     = InkOf(Cell.Ink, Screen.Text);
    Made.Mark      = InkOf(Cell.MarkInk, Screen.Text);
    Made.Caret     = Made.Glyph.Alpha != 0 && Made.Glyph.Rgb != Made.Paper.Rgb ? Ink{Made.Glyph.Rgb, 255} : Ink{COLOR_LIGHT_GRAY, 255};
    Made.Under     = Cell.Mark == ITEM_MARK_UNDER;
    Made.Bar       = Cell.Mark == ITEM_MARK_BAR;
    Made.CaretHere = CaretHere;
    Made.Picture   = &Screen.Type->Glyph(Cell.Character);
    return Made;
}

/* What covers pixel X of row Y of a cell: the caret or a mark, wholly — or
 * the glyph, by its share of the pixel (Share, 0..255), over the paper. */
enum class Over : std::uint8_t { Glyph, Mark, Caret };

struct Row {
    Over                What  = Over::Glyph;
    const std::uint8_t *Shares = nullptr;   // the glyph's coverage along the row, none where it has none
};

/* The caret and a line under the glyph: an eighth of the cell at its foot,
 * two rows at least. A bar: a quarter of its width, two pixels at least. */
Row RowOf(const Look &Of, const typecase::Size &Fit, std::int32_t Y) noexcept
{
    const std::int32_t Foot = Fit.Height - std::max(2, Fit.Height / 8);
    if (Y >= Foot && (Of.CaretHere || Of.Under)) return Row{Of.CaretHere ? Over::Caret : Over::Mark, nullptr};
    /* Where the picture's first row stands in the cell: it hangs from the pen
     * on the cell's baseline. */
    const typecase::GlyphImage &Picture = *Of.Picture;
    const std::int32_t          Into    = Y - (Fit.Baseline + Picture.Top);
    if (Into < 0 || Into >= Picture.Height) return Row{};
    return Row{Over::Glyph, Picture.Coverage.data() + static_cast<std::size_t>(Into) * static_cast<std::size_t>(Picture.Width)};
}

std::uint8_t ShareAt(const Look &Of, const Row &Line, std::int32_t X) noexcept
{
    const std::int32_t Along = X - Of.Picture->Left;
    return Line.Shares && Along >= 0 && Along < Of.Picture->Width ? Line.Shares[Along] : 0;
}

/* Whether a cell covers its pixels whole: every colour it draws in opaque,
 * the whole of it laid at once. */
bool Plain(const Look &Of, std::uint32_t Opacity) noexcept
{
    return Opacity == Whole && Of.Paper.Alpha == 255 && Of.Glyph.Alpha == 255 && (Of.Mark.Alpha == 255 || !(Of.Under || Of.Bar));
}

/* Row Y of a cell that covers its pixels whole, its pixels [From, To) as
 * triples into Out: each the triple the ink and the paper mix to by the share
 * of it the glyph covers (the system's one rule, BoxColorOver), a mark's or
 * the caret's where they lie — what the glass shows. The one rule every
 * opaque cell is drawn by, into a canvas or straight into the mirror. */
void RowTriples(const Look &Of, const typecase::Size &Fit, std::int32_t Y, std::int32_t From, std::int32_t To,
                std::uint32_t *Out) noexcept
{
    const std::int32_t BarWide = std::max(2, Fit.Width / 4);
    const Row          Line    = RowOf(Of, Fit, Y);
    for (std::int32_t X = From; X < To; X++) {
        if (Line.What == Over::Caret) *Out++ = Of.Caret.Rgb;
        else if (Line.What == Over::Mark || (Of.Bar && X < BarWide)) *Out++ = Of.Mark.Rgb;
        else {
            const std::uint8_t Share = ShareAt(Of, Line, X);
            *Out++ = Share == 0 ? Of.Paper.Rgb : Share == 255 ? Of.Glyph.Rgb : BoxColorOver(Of.Paper.Rgb, Of.Glyph.Rgb, Share);
        }
    }
}

/* One cell's part Part (target pixels) drawn, the cell's corner at Corner. */
void DrawCell(const Look &Of, const typecase::Size &Fit, painter::core::Canvas &Target, painter::core::Point Corner, Rect Part,
              std::uint32_t Opacity)
{
    const std::int32_t BarWide = std::max(2, Fit.Width / 4);
    if (Plain(Of, Opacity)) {
        /* Its triples, as light: the paper's and the ink's worked out once. */
        static std::vector<std::uint32_t> Triples;
        if (Triples.size() < static_cast<std::size_t>(Part.Width)) Triples.resize(static_cast<std::size_t>(Part.Width));
        const Pixel Paper = PixelOf(Of.Paper), Glyph = PixelOf(Of.Glyph);
        for (std::int32_t Y = Part.Y; Y < Part.Y + Part.Height; Y++) {
            RowTriples(Of, Fit, Y - Corner.Y, Part.X - Corner.X, Part.X + Part.Width - Corner.X, Triples.data());
            Pixel *Out = Target.Row(static_cast<std::uint32_t>(Y)) + Part.X;
            for (std::int32_t X = 0; X < Part.Width; X++) {
                const std::uint32_t Triple = Triples[static_cast<std::size_t>(X)];
                Out[X] = Triple == Of.Paper.Rgb ? Paper : Triple == Of.Glyph.Rgb ? Glyph : PixelOf(Ink{Triple, 255});
            }
        }
        return;
    }
    /* Something lets what lies beneath show: the paper laid over it, then
     * the glyph by its share — or a mark, or the caret — each as light. */
    const Pixel Paper = PixelOf(Of.Paper), Glyph = PixelOf(Of.Glyph), Mark = PixelOf(Of.Mark), Caret = PixelOf(Of.Caret);
    const auto  Times = [](std::uint32_t A, std::uint32_t B) {
        const std::uint32_t X = A * B + 32768u;
        return (X + (X >> 16)) >> 16;
    };
    for (std::int32_t Y = Part.Y; Y < Part.Y + Part.Height; Y++) {
        const Row Line = RowOf(Of, Fit, Y - Corner.Y);
        Pixel    *Out  = Target.Row(static_cast<std::uint32_t>(Y)) + Part.X;
        for (std::int32_t X = 0; X < Part.Width; X++) {
            const std::int32_t In = Part.X + X - Corner.X;
            Pixel              P  = Of.Paper.Alpha ? Pixel::Over(Out[X], Paper, Opacity) : Out[X];
            if (Line.What == Over::Caret) P = Pixel::Over(P, Caret, Opacity);
            else if (Line.What == Over::Mark || (Of.Bar && In < BarWide)) P = Pixel::Over(P, Mark, Opacity);
            else if (const std::uint8_t Share = ShareAt(Of, Line, In); Share != 0 && Of.Glyph.Alpha != 0)
                P = Pixel::Over(P, Glyph, Times(Share * 257u, Opacity));
            Out[X] = P;
        }
    }
}

}  // namespace

void DrawCells(const CellGrid &Grid, const CellsLook &Screen, painter::core::Canvas &Target, painter::core::Point Corner,
               float Opacity)
{
    if (!Screen.Type || Grid.Cols() == 0 || Grid.Rows() == 0 || !(Opacity > 0)) return;
    const typecase::Size &Fit  = Screen.Type->Cell();
    const Rect            Clip = Target.Clip().Intersect(
        Rect{Corner.X, Corner.Y, static_cast<std::int32_t>(Grid.Cols()) * Fit.Width, static_cast<std::int32_t>(Grid.Rows()) * Fit.Height});
    if (Clip.Empty()) return;
    const auto Share = static_cast<std::uint32_t>(std::lround(std::min(Opacity, 1.0f) * static_cast<float>(Whole)));
    const auto First = [](std::int32_t From, std::int32_t Step) { return static_cast<std::uint32_t>(From / Step); };
    const std::uint32_t FirstCol = First(Clip.X - Corner.X, Fit.Width), LastCol = First(Clip.X + Clip.Width - 1 - Corner.X, Fit.Width);
    const std::uint32_t FirstRow = First(Clip.Y - Corner.Y, Fit.Height), LastRow = First(Clip.Y + Clip.Height - 1 - Corner.Y, Fit.Height);
    for (std::uint32_t Row = FirstRow; Row <= LastRow; Row++)
        for (std::uint32_t Col = FirstCol; Col <= LastCol; Col++) {
            const painter::core::Point At{Corner.X + static_cast<std::int32_t>(Col) * Fit.Width,
                                          Corner.Y + static_cast<std::int32_t>(Row) * Fit.Height};
            const Rect Part = Rect{At.X, At.Y, Fit.Width, Fit.Height}.Intersect(Clip);
            const bool Caret = Grid.CaretAt(Col, Row);
            /* A cell that shows nothing draws nothing: what lies beneath it
             * shows. */
            if (Part.Empty() || (!Caret && !CellShows(Grid.At(Col, Row)))) continue;
            DrawCell(LookOf(Grid.At(Col, Row), Screen, Caret), Fit, Target, At, Part, Share);
        }
}

void LayCells(const CellGrid &Grid, const CellsLook &Screen, std::uint32_t *Pixels, std::size_t Stride, Rect Area,
              painter::core::Point Corner) noexcept
{
    if (!Screen.Type || Grid.Cols() == 0 || Grid.Rows() == 0) return;
    const typecase::Size &Fit  = Screen.Type->Cell();
    const Rect            Clip = Area.Intersect(
        Rect{Corner.X, Corner.Y, static_cast<std::int32_t>(Grid.Cols()) * Fit.Width, static_cast<std::int32_t>(Grid.Rows()) * Fit.Height});
    if (Clip.Empty()) return;
    const auto          First    = [](std::int32_t From, std::int32_t Step) { return static_cast<std::uint32_t>(From / Step); };
    const std::uint32_t FirstCol = First(Clip.X - Corner.X, Fit.Width), LastCol = First(Clip.X + Clip.Width - 1 - Corner.X, Fit.Width);
    const std::uint32_t FirstRow = First(Clip.Y - Corner.Y, Fit.Height), LastRow = First(Clip.Y + Clip.Height - 1 - Corner.Y, Fit.Height);
    for (std::uint32_t Row = FirstRow; Row <= LastRow; Row++)
        for (std::uint32_t Col = FirstCol; Col <= LastCol; Col++) {
            const painter::core::Point At{Corner.X + static_cast<std::int32_t>(Col) * Fit.Width,
                                          Corner.Y + static_cast<std::int32_t>(Row) * Fit.Height};
            const Rect Part = Rect{At.X, At.Y, Fit.Width, Fit.Height}.Intersect(Clip);
            if (Part.Empty()) continue;
            const Look Of = LookOf(Grid.At(Col, Row), Screen, Grid.CaretAt(Col, Row));
            for (std::int32_t Y = Part.Y; Y < Part.Y + Part.Height; Y++)
                RowTriples(Of, Fit, Y - At.Y, Part.X - At.X, Part.X + Part.Width - At.X,
                           Pixels + static_cast<std::size_t>(Y - Area.Y) * Stride + static_cast<std::size_t>(Part.X - Area.X));
        }
}

void PageCells(const CellGrid &Grid, std::int32_t Col, std::int32_t Row, std::span<ItemCell> Cells, std::uint32_t Cols,
               std::uint32_t Rows, std::uint32_t Page) noexcept
{
    if (Cells.size() != static_cast<std::size_t>(Cols) * Rows) return;
    for (std::uint32_t Y = 0; Y < Grid.Rows(); Y++) {
        const std::int64_t PageRow = static_cast<std::int64_t>(Row) + Y;
        if (PageRow < 0 || PageRow >= Rows) continue;
        for (std::uint32_t X = 0; X < Grid.Cols(); X++) {
            const std::int64_t PageCol = static_cast<std::int64_t>(Col) + X;
            if (PageCol < 0 || PageCol >= Cols) continue;
            const ItemCell &Above   = Grid.At(X, Y);
            ItemCell       &Beneath = Cells[static_cast<std::size_t>(PageRow) * Cols + static_cast<std::size_t>(PageCol)];
            ItemCell        Shown;
            const bool      Covers = CellOver(Above, Beneath, Page, Shown);
            if (Above.Mark != ITEM_MARK_NONE) {
                Shown.Mark    = Above.Mark;
                Shown.MarkInk = Above.MarkInk;
                Beneath       = Shown;
            } else if (Covers) {
                Shown.Mark = ITEM_MARK_NONE;
                Beneath    = Shown;
            }
        }
    }
}

painter::core::Point OnTheGrid(const CellsLook &Screen, painter::core::Point Corner) noexcept
{
    const auto After = [](std::int32_t Grid, std::int32_t From, std::int32_t Step) {
        const std::int32_t Apart = (Grid - From) % Step;
        return From + (Apart < 0 ? Apart + Step : Apart);
    };
    return painter::core::Point{After(Screen.Grid.X, Corner.X, Screen.Width), After(Screen.Grid.Y, Corner.Y, Screen.Height)};
}
