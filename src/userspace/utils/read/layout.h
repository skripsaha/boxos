#ifndef READ_LAYOUT_H
#define READ_LAYOUT_H

/* The book in rows of a column so many cells wide. A paragraph is wrapped at
 * the blanks between its words; code and tables keep their lines and are cut
 * at the column's edge where they are drawn; a quote stands behind its bar
 * and an item behind its marker, wrapped under its own first word; a heading
 * of the first two levels is underlined; a rule is a line across. A blank row
 * stands between blocks, except between the items of one list and between
 * the lines of a file read as lines.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

#include "book.h"

namespace read {

enum class RowKind : std::uint8_t { Text, Space, Underline, RuleLine };

struct Row {
    RowKind      Is     = RowKind::Space;
    std::size_t  Block  = 0;      /* a blank row's: the block after it */
    std::size_t  From   = 0;      /* bytes [From, To) of the block's text */
    std::size_t  To     = 0;
    std::int32_t Indent = 0;      /* cells from the column's left edge to the text */
    bool         First  = false;  /* the block's first row: its marker or bar stands here */
};

class Layout {
public:
    void Make(const Book &Of, std::int32_t Width);

    const std::vector<Row> &Rows() const noexcept { return All; }
    std::int32_t            Width() const noexcept { return Cols; }
    /* The row that shows byte Byte of block Block's text. */
    std::size_t             RowOf(std::size_t Block, std::size_t Byte = 0) const noexcept;
    /* Where an item's marker or a quote's bar stands, from the column's left
     * edge. */
    static std::int32_t     MarkerAt(const Block &One) noexcept;

private:
    void Wrap(const Block &One, std::size_t Index, std::int32_t Indent);
    void Keep(const Block &One, std::size_t Index);

    std::vector<Row>         All;
    std::vector<std::size_t> FirstRow;
    std::int32_t             Cols = 0;
};

}  // namespace read

#endif
