#ifndef READ_TAB_H
#define READ_TAB_H

/* One open book: the file read as a book and laid in the column, the reading
 * line and the selection, which rows are on the page, and its bookmarks.
 *
 * The reading line is the line read stands on, as edit's caret is: the arrows
 * move it, the page follows it, a bookmark is put on it, and with Shift it
 * selects whole lines from where it was. It only ever stands on a row of
 * words. Rows is the page's rows, said to every move, since what the page
 * holds is the glass's.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "book.h"
#include "layout.h"
#include "marks.h"

namespace read {

class Tab {
public:
    Tab(Book Opened, std::string Tags);

    Book              Body;
    std::string       Tags;       /* as the volume says them, comma-joined */
    Layout            Lines;
    std::vector<Mark> Marked;     /* its bookmarks, first in the file first */

    std::size_t                Top     = 0;   /* the first row on the page */
    std::size_t                Reading = 0;   /* the reading line */
    std::optional<std::size_t> Anchor;        /* the other end of the selection */

    /* Laid in a column Width wide — again only when it was laid at another
     * width — the reading line kept on the same words, as far down the page
     * as it was. */
    void Lay(std::int32_t Width, std::size_t Rows);
    /* The same file read the other way, the reading line on the same words. */
    void Turn(std::int32_t Width, std::size_t Rows);

    bool        TextRow(std::size_t Row) const noexcept;
    std::size_t MostTop(std::size_t Rows) const noexcept;
    /* Where the reading line's paragraph begins in the file. */
    std::size_t PlaceOfReading() const noexcept;
    std::size_t BlockOfReading() const noexcept;

    bool                                HasSelection() const noexcept { return Anchor && *Anchor != Reading; }
    std::pair<std::size_t, std::size_t> Selection() const noexcept;

    /* Moving. Extend keeps the anchor where it was, so the selection grows. */
    void Step(bool Down, bool Extend, std::size_t Rows);
    /* A page on, or back — one row of the old page kept, so the eye has where
     * it was — the reading line at the top of the new one; at the end of the
     * book, its last line. */
    void Page(bool Down, bool Extend, std::size_t Rows);
    void Edge(bool End, std::size_t Rows);
    /* The next heading, or the one before; false when there is none that way. */
    bool Heading(bool Next, std::size_t Rows);
    /* The reading line on the block, near the top of the page: one row of
     * what is above it kept. */
    void Land(std::size_t Block, std::size_t Rows);
    void SelectAll(std::size_t Rows);
    /* The reading line on that byte of that block; the page moved about it
     * — a third of the way down, where the eye goes — when it is off the page
     * or among its first Covered rows, which a window stands over. */
    void GoTo(std::size_t Block, std::size_t Byte, std::size_t Rows, std::size_t Covered = 0);
    void Follow(std::size_t Rows) noexcept;

    /* A page turns by its rows less one: pages are counted by the same step. */
    static std::size_t Stride(std::size_t Rows) noexcept { return Rows > 1 ? Rows - 1 : 1; }
    std::size_t        Pages(std::size_t Rows) const noexcept;
    std::size_t        PageOf(std::size_t Row, std::size_t Rows) const noexcept;
};

}  // namespace read

#endif
