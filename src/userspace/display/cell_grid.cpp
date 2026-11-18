/*
 * cell_grid.cpp — a grid of cells and what changed in it. See cell_grid.h.
 */

#include "cell_grid.h"

#include <algorithm>
#include <cstring>

#include "box/color.h"

namespace {

bool Same(const ItemCell &A, const ItemCell &B) noexcept { return std::memcmp(&A, &B, sizeof(ItemCell)) == 0; }

}  // namespace

bool SeeThroughPaper(std::uint32_t Paper) noexcept
{
    return Paper == COLOR_THROUGH || BoxColorIsTranslucent(Paper);
}

bool CellShows(const ItemCell &Cell) noexcept
{
    return BoxColorOpacity(Cell.Ink) != 0 || BoxColorOpacity(Cell.Paper) != 0 || Cell.Mark != ITEM_MARK_NONE;
}

CellGrid::CellGrid(std::uint32_t Cols, std::uint32_t Rows, const ItemCell &Blank)
    : Wide(Cols), High(Rows), All(static_cast<std::size_t>(Cols) * Rows, Blank), Marks(Rows)
{
    SeeThrough = SeeThroughPaper(Blank.Paper) ? All.size() : 0;
    MarkAll();
}

void CellGrid::Counted(const ItemCell &Was, const ItemCell &Now) noexcept
{
    SeeThrough = SeeThrough - SeeThroughPaper(Was.Paper) + SeeThroughPaper(Now.Paper);
    ShowsKnown = false;
}

painter::core::Rect CellGrid::Showing() const noexcept
{
    if (ShowsKnown) return Shows;
    std::uint32_t Left = Wide, Top = High, Right = 0, Bottom = 0;
    for (std::uint32_t Row = 0; Row < High; Row++)
        for (std::uint32_t Col = 0; Col < Wide; Col++) {
            if (!CellShows(At(Col, Row))) continue;
            Left = std::min(Left, Col), Right = std::max(Right, Col + 1);
            Top = std::min(Top, Row), Bottom = std::max(Bottom, Row + 1);
        }
    Shows      = Left < Right ? painter::core::Rect{static_cast<std::int32_t>(Left), static_cast<std::int32_t>(Top),
                                                    static_cast<std::int32_t>(Right - Left), static_cast<std::int32_t>(Bottom - Top)}
                              : painter::core::Rect{};
    ShowsKnown = true;
    return Shows;
}

void CellGrid::Mark(std::uint32_t Col, std::uint32_t Row, std::uint32_t Count) noexcept
{
    if (Row >= High || Col >= Wide || Count == 0) return;
    const std::uint32_t End  = Col + std::min(Count, Wide - Col);
    auto               &Mark = Marks[Row];
    if (Mark.first == Mark.second) {
        Mark = {Col, End};
        MarkedRows++;
        return;
    }
    Mark = {std::min(Mark.first, Col), std::max(Mark.second, End)};
}

void CellGrid::MarkAll() noexcept
{
    for (auto &Mark : Marks) Mark = {0, Wide};
    MarkedRows = Wide ? High : 0;
}

void CellGrid::Put(std::size_t Index, const ItemCell &Cell) noexcept
{
    if (Index >= All.size()) return;
    const auto Col = static_cast<std::uint32_t>(Index % Wide), Row = static_cast<std::uint32_t>(Index / Wide);
    ItemCell  &Into = All[Kept(Row) + Col];
    if (Same(Into, Cell)) return;
    Counted(Into, Cell);
    Into = Cell;
    Mark(Col, Row, 1);
}

void CellGrid::CopyOut(std::size_t From, std::size_t To, std::vector<ItemCell> &Into) const
{
    To = std::min(To, All.size());
    Into.clear();
    for (std::size_t Index = From; Index < To; Index++) Into.push_back(At(Index));
}

void CellGrid::Fill(std::size_t From, std::size_t To, const ItemCell &Cell) noexcept
{
    To = std::min(To, All.size());
    for (std::size_t Index = From; Index < To; Index++) Put(Index, Cell);
}

void CellGrid::Write(std::uint32_t Row, std::uint32_t Col, std::span<const ItemCell> Run) noexcept
{
    if (Row >= High || Col >= Wide) return;
    const std::size_t Start = static_cast<std::size_t>(Row) * Wide + Col;
    const std::size_t Count = std::min<std::size_t>(Run.size(), Wide - Col);
    for (std::size_t I = 0; I < Count; I++) Put(Start + I, Run[I]);
}

void CellGrid::Scroll(std::uint32_t Count, const ItemCell &Blank) noexcept
{
    if (Count == 0 || Wide == 0) return;
    Count = std::min(Count, High);
    /* The rows that go come in again at the foot, blank: the ring turned. */
    for (std::uint32_t Row = 0; Row < Count; Row++) {
        ItemCell *Line = All.data() + Kept(Row);
        for (std::uint32_t Col = 0; Col < Wide; Col++) {
            Counted(Line[Col], Blank);
            Line[Col] = Blank;
        }
    }
    Top = Top + Count >= High ? Top + Count - High : Top + Count;
    /* What was marked moved up with its cells; the rows come in marked. */
    for (std::uint32_t Row = 0; Row < Count; Row++) MarkedRows -= Marks[Row].first != Marks[Row].second;
    std::move(Marks.begin() + Count, Marks.end(), Marks.begin());
    for (std::uint32_t Row = High - Count; Row < High; Row++) {
        Marks[Row] = {0, Wide};
        MarkedRows++;
    }
    /* The caret's picture went up with the text: where it now stands is
     * where it is taken from when it moves again. */
    Said.Row = Said.Row >= Count ? Said.Row - Count : 0;
    Moved    = std::min(Moved + Count, High);
}

void CellGrid::SetCaret(std::uint32_t Col, std::uint32_t Row, bool Shown) noexcept
{
    const ItemCaret Now{Col, Row, static_cast<std::uint8_t>(Shown), {}};
    if (Now.Col == Said.Col && Now.Row == Said.Row && Now.Shown == Said.Shown) return;
    if (CaretShown()) Mark(Said.Col, Said.Row, 1);
    Said = Now;
    if (CaretShown()) Mark(Said.Col, Said.Row, 1);
}

void CellGrid::HoldCaret(bool Holding) noexcept
{
    if (Holding == Held) return;
    Held = Holding;
    if (CaretShown()) Mark(Said.Col, Said.Row, 1);
}

std::uint32_t CellGrid::TakeScrolled() noexcept
{
    const std::uint32_t Taken = Moved;
    Moved                     = 0;
    return Taken;
}

bool CellOver(const ItemCell &Above, const ItemCell &Beneath, std::uint32_t Page, ItemCell &Shown) noexcept
{
    if (BoxColorOpacity(Above.Ink) == 0 && BoxColorOpacity(Above.Paper) == 0) {
        Shown = Beneath;
        return false;
    }
    const std::uint32_t Seen     = (Beneath.Paper >> 24) ? Page : Beneath.Paper;
    const auto          Resolved = [Seen](std::uint32_t Colour) {
        if (Colour == COLOR_THROUGH) return Seen;
        return BoxColorIsTranslucent(Colour) ? BoxColorBlendOver(Colour, Seen) : Colour;
    };
    Shown       = Above;
    Shown.Ink   = Resolved(Above.Ink);
    Shown.Paper = Resolved(Above.Paper);
    return true;
}
