/* One open book: laying it in the column, and moving the reading line. */

#include "tab.h"

#include <algorithm>

namespace read {

Tab::Tab(Book Opened, std::string TagsValue) : Body(std::move(Opened)), Tags(std::move(TagsValue)) {}

void Tab::Lay(std::int32_t Width, std::size_t Rows)
{
    if (Lines.Width() == Width) return;
    if (Lines.Rows().empty()) {
        Lines.Make(Body, Width);
        Edge(false, Rows);
        return;
    }
    const Row         Was  = Lines.Rows()[Reading];
    const std::size_t Down = Reading - std::min(Reading, Top);
    Lines.Make(Body, Width);
    Anchor.reset();
    if (Lines.Rows().empty()) return;
    Reading = Lines.RowOf(Was.Block, Was.From);
    Top     = std::min(Reading > Down ? Reading - Down : 0, MostTop(Rows));
}

void Tab::Turn(std::int32_t Width, std::size_t Rows)
{
    const std::size_t Place = PlaceOfReading();
    Body                    = Body.Other();
    Lines.Make(Body, Width);
    Top = Reading = 0;
    GoTo(Body.BlockAt(Place), 0, Rows);
}

bool Tab::TextRow(std::size_t Row) const noexcept
{
    return Row < Lines.Rows().size() && Lines.Rows()[Row].Is == RowKind::Text;
}

std::size_t Tab::MostTop(std::size_t Rows) const noexcept
{
    const std::size_t All = Lines.Rows().size();
    return All > Rows ? All - Rows : 0;
}

std::size_t Tab::BlockOfReading() const noexcept
{
    return Lines.Rows().empty() ? 0 : Lines.Rows()[Reading].Block;
}

std::size_t Tab::PlaceOfReading() const noexcept
{
    if (Lines.Rows().empty() || Body.Blocks.empty()) return 0;
    return Body.Blocks[std::min(BlockOfReading(), Body.Blocks.size() - 1)].Source;
}

std::pair<std::size_t, std::size_t> Tab::Selection() const noexcept
{
    const std::size_t Other = Anchor.value_or(Reading);
    return {std::min(Other, Reading), std::max(Other, Reading)};
}

std::size_t Tab::Pages(std::size_t Rows) const noexcept
{
    return (MostTop(Rows) + Stride(Rows) - 1) / Stride(Rows) + 1;
}

std::size_t Tab::PageOf(std::size_t Row, std::size_t Rows) const noexcept
{
    return std::min(Row / Stride(Rows) + 1, Pages(Rows));
}

void Tab::Follow(std::size_t Rows) noexcept
{
    if (Reading < Top) Top = Reading;
    if (Reading >= Top + Rows) Top = Reading - Rows + 1;
}

void Tab::Step(bool Down, bool Extend, std::size_t Rows)
{
    if (Extend && !Anchor) Anchor = Reading;
    if (!Extend) Anchor.reset();
    for (std::size_t Row = Reading; Down ? Row + 1 < Lines.Rows().size() : Row > 0;) {
        Row = Down ? Row + 1 : Row - 1;
        if (TextRow(Row)) {
            Reading = Row;
            break;
        }
    }
    Follow(Rows);
}

void Tab::Page(bool Down, bool Extend, std::size_t Rows)
{
    if (Lines.Rows().empty()) return;
    if (Extend && !Anchor) Anchor = Reading;
    if (!Extend) Anchor.reset();

    const std::size_t Most = MostTop(Rows), By = Stride(Rows);
    if (Down ? Top >= Most : Top == 0) {
        const std::optional<std::size_t> Kept = Anchor;
        Edge(Down, Rows);
        Anchor = Kept;
        return;
    }
    Top = Down ? std::min(Top + By, Most) : (Top > By ? Top - By : 0);
    for (Reading = Top; Reading + 1 < Lines.Rows().size() && !TextRow(Reading);) Reading++;
}

void Tab::Edge(bool End, std::size_t Rows)
{
    Anchor.reset();
    Top = Reading = 0;
    if (Lines.Rows().empty()) return;
    Top = End ? MostTop(Rows) : 0;
    if (End) {
        for (Reading = Lines.Rows().size() - 1; Reading > 0 && !TextRow(Reading);) Reading--;
    } else {
        for (Reading = 0; Reading + 1 < Lines.Rows().size() && !TextRow(Reading);) Reading++;
    }
}

/* From a heading, the one before is the heading before it; from inside a
 * part, the heading the part is under. */
bool Tab::Heading(bool Next, std::size_t Rows)
{
    if (Lines.Rows().empty()) return false;
    const std::size_t          Block = BlockOfReading();
    std::optional<std::size_t> To;
    for (const std::size_t One : Body.Headings()) {
        if (Next && One > Block) {
            To = One;
            break;
        }
        if (!Next && One < Block) To = One;
    }
    if (!To) return false;
    Land(*To, Rows);
    return true;
}

void Tab::Land(std::size_t Block, std::size_t Rows)
{
    if (Lines.Rows().empty()) return;
    Anchor.reset();
    Reading = Lines.RowOf(Block);
    Top     = std::min(Reading > 0 ? Reading - 1 : 0, MostTop(Rows));
}

void Tab::SelectAll(std::size_t Rows)
{
    Edge(false, Rows);
    if (Lines.Rows().empty()) return;
    Anchor = Reading;
    for (Reading = Lines.Rows().size() - 1; Reading > 0 && !TextRow(Reading);) Reading--;
    Follow(Rows);
}

void Tab::GoTo(std::size_t Block, std::size_t Byte, std::size_t Rows, std::size_t Covered)
{
    if (Lines.Rows().empty()) return;
    Reading = Lines.RowOf(Block, Byte);
    Anchor.reset();
    if (Reading < Top + Covered || Reading >= Top + Rows)
        Top = std::min(Reading > Rows / 3 ? Reading - Rows / 3 : 0, MostTop(Rows));
}

}  // namespace read
