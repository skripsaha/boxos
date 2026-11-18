#ifndef BOXOS_DISPLAY_CELL_GRID_H
#define BOXOS_DISPLAY_CELL_GRID_H

// A grid of cells (box/items.h ItemCell) — the console's, or a program's
// cells item: what each cell holds, where the caret stands, and what changed
// since a frame last took it — each row's stretch, and how many rows the grid
// scrolled up meanwhile, so a frame moves what the glass shows already rather
// than drawing it again. Cells are counted from the first, row after row.

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "box/items.h"

class CellGrid {
public:
    // Cols x Rows cells, each Blank, all of them to be drawn. Throws
    // std::bad_alloc only.
    CellGrid(std::uint32_t Cols, std::uint32_t Rows, const ItemCell &Blank);

    std::uint32_t Cols() const noexcept { return Wide; }
    std::uint32_t Rows() const noexcept { return High; }
    std::span<const ItemCell> Cells() const noexcept { return All; }
    const ItemCell &At(std::uint32_t Col, std::uint32_t Row) const noexcept
    {
        return All[static_cast<std::size_t>(Row) * Wide + Col];
    }

    // A cell; the cells [From, To); a run along a row from Col, as far as the
    // row goes; every cell — each marked where it changes.
    void Put(std::size_t Index, const ItemCell &Cell) noexcept;
    void Fill(std::size_t From, std::size_t To, const ItemCell &Cell) noexcept;
    void Write(std::uint32_t Row, std::uint32_t Col, std::span<const ItemCell> Run) noexcept;

    // Count rows scrolled up off the grid, as many rows of Blank coming in at
    // its foot; the caret with them.
    void Scroll(std::uint32_t Count, const ItemCell &Blank) noexcept;

    // The caret: where it stands and whether it is shown, as its owner says;
    // and whether it is the one caret the glass shows (Held). The cells it
    // leaves and comes to are marked.
    void SetCaret(std::uint32_t Col, std::uint32_t Row, bool Shown) noexcept;
    void HoldCaret(bool Held) noexcept;
    ItemCaret Caret() const noexcept { return Said; }
    bool      CaretShown() const noexcept { return Said.Shown != 0 && Said.Col < Wide && Said.Row < High; }
    bool      Holds() const noexcept { return Held; }
    // Whether the caret the glass shows is drawn in that cell.
    bool CaretAt(std::uint32_t Col, std::uint32_t Row) const noexcept
    {
        return Held && CaretShown() && Said.Col == Col && Said.Row == Row;
    }

    // Whether every cell's paper covers what lies beneath it.
    bool Opaque() const noexcept { return SeeThrough == 0; }

    // Whether anything changed since it was last taken.
    bool Changed() const noexcept { return MarkedRows != 0 || Moved != 0; }
    // The rows it scrolled up since last asked — Rows() or more when every
    // cell has changed anyway — and none from now on.
    std::uint32_t TakeScrolled() noexcept;
    // Each row's changed stretch told to Each(Row, From, To), and none
    // changed from now on.
    template <typename Stretch>
    void TakeMarks(Stretch &&Each)
    {
        for (std::uint32_t Row = 0; Row < High && MarkedRows != 0; Row++) {
            auto &Mark = Marks[Row];
            if (Mark.first == Mark.second) continue;
            const std::pair<std::uint32_t, std::uint32_t> Taken = Mark;
            Mark                                               = {0, 0};
            MarkedRows--;
            Each(Row, Taken.first, Taken.second);
        }
    }
    // Every cell to be drawn again.
    void MarkAll() noexcept;
    // Count cells from Col of Row to be drawn again.
    void Mark(std::uint32_t Col, std::uint32_t Row, std::uint32_t Count) noexcept;

private:
    void Counted(const ItemCell &Was, const ItemCell &Now) noexcept;

    std::uint32_t                                       Wide = 0, High = 0;
    std::vector<ItemCell>                               All;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> Marks;   // each row's [From, To)
    std::uint32_t                                       MarkedRows = 0;
    std::uint32_t                                       Moved      = 0;   // rows scrolled up since asked
    std::size_t                                         SeeThrough = 0;   // cells whose paper lets through
    ItemCaret                                           Said{};
    bool                                                Held = false;
};

// Where the caret stands on a page of cells, and whether it is shown.
struct Caret {
    std::uint32_t X = 0;
    std::uint32_t Y = 0;
    bool Visible = false;
};

// Whether a cell's paper lets what lies beneath it show: see-through, or a
// colour at an opacity.
bool SeeThroughPaper(std::uint32_t Paper) noexcept;

// What a cell shows laid over the cell beneath it on a page of cells: its
// own colours, each COLOR_THROUGH one the paper of the cell beneath and each
// translucent one laid over that paper (BoxColorBlendOver) — Page where the
// paper beneath is the screen's own, for COLOR_DEFAULT would be drawn as the
// text colour in an ink. False when both of its colours are wholly
// see-through and the cell covers nothing: Shown is the cell beneath then.
// The mark is not Over's to lay: a cell's mark is drawn whatever its colours.
bool CellOver(const ItemCell &Above, const ItemCell &Beneath, std::uint32_t Page, ItemCell &Shown) noexcept;

#endif
