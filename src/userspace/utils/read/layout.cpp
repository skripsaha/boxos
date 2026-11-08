/* The book in rows of a column. */

#include "layout.h"

#include <algorithm>

#include "kin/glyph.h"

namespace read {

using namespace kin;

namespace {

/* Whether a blank row stands before a block of kind Now that follows one of
 * kind Before. */
bool SpaceBetween(Kind Before, Kind Now) noexcept
{
    if (Now == Kind::Line) return false;
    return !(Before == Kind::Item && Now == Kind::Item);
}

}  // namespace

std::int32_t Layout::MarkerAt(const Block &One) noexcept
{
    if (One.Is == Kind::Item) return 2 * One.Level;
    if (One.Is == Kind::Quote) return 2 * (One.Level > 0 ? One.Level - 1 : 0);
    return 0;
}

void Layout::Make(const Book &Of, std::int32_t Width)
{
    Cols = std::max(Width, 8);
    All.clear();
    FirstRow.assign(Of.Blocks.size(), 0);

    for (std::size_t Index = 0; Index < Of.Blocks.size(); Index++) {
        const Block &One = Of.Blocks[Index];
        if (Index > 0 && SpaceBetween(Of.Blocks[Index - 1].Is, One.Is)) All.push_back({RowKind::Space, Index});
        FirstRow[Index] = All.size();

        switch (One.Is) {
        case Kind::Code:
        case Kind::Table: Keep(One, Index); break;
        case Kind::Rule:  All.push_back({RowKind::RuleLine, Index, 0, 0, 0, true}); break;
        case Kind::Quote: Wrap(One, Index, MarkerAt(One) + 2); break;
        case Kind::Item: {
            const std::int32_t Marker = One.Marker.empty() ? 1 : static_cast<std::int32_t>(CellsOf(One.Marker));
            Wrap(One, Index, MarkerAt(One) + Marker + 1);
            break;
        }
        default: Wrap(One, Index, 0); break;
        }

        if (One.Is == Kind::Heading && One.Level <= 2) All.push_back({RowKind::Underline, Index});
    }
}

/* Words to a row, as many as the column holds; a word longer than the column
 * is cut where the column ends. A line read as a line is cut wherever it
 * reaches the edge, every character kept. */
void Layout::Wrap(const Block &One, std::size_t Index, std::int32_t Indent)
{
    const std::string_view Text  = One.Text;
    const std::size_t      Room  = static_cast<std::size_t>(std::max(Cols - Indent, 4));
    const bool             Words = One.Is != Kind::Line;
    std::size_t            At    = 0;
    bool                   First = true;
    do {
        std::size_t End = At + BytesWithin(Text.substr(At), Room);
        std::size_t Next = End;
        if (End < Text.size()) {
            const std::size_t Blank = Words ? Text.rfind(' ', End) : std::string_view::npos;
            if (Blank != std::string_view::npos && Blank > At) {
                End  = Blank;
                Next = Blank + 1;
            } else if (End == At) {
                End  = At + GlyphAt(Text, At).Length;
                Next = End;
            }
        }
        All.push_back({RowKind::Text, Index, At, End, Indent, First});
        First = false;
        At    = Next;
        while (Words && At < Text.size() && Text[At] == ' ') At++;
    } while (At < Text.size());
}

/* Code and tables: a row a line, as the file has them. */
void Layout::Keep(const Block &One, std::size_t Index)
{
    std::size_t At    = 0;
    bool        First = true;
    for (;;) {
        const std::size_t End = std::min(One.Text.find('\n', At), One.Text.size());
        All.push_back({RowKind::Text, Index, At, End, 0, First});
        First = false;
        if (End >= One.Text.size()) break;
        At = End + 1;
    }
}

std::size_t Layout::RowOf(std::size_t Block, std::size_t Byte) const noexcept
{
    if (All.empty()) return 0;
    if (Block >= FirstRow.size()) return All.size() - 1;
    std::size_t Found = FirstRow[Block];
    for (std::size_t Row = Found; Row < All.size() && All[Row].Block == Block; Row++) {
        if (All[Row].Is != RowKind::Text) continue;
        if (All[Row].From <= Byte) Found = Row;
    }
    return Found;
}

}  // namespace read
