#ifndef READ_MARKS_H
#define READ_MARKS_H

/* What read remembers from one reading to the next, in a file of its own on
 * the volume, read.marks: where each book was left, its bookmarks, and how
 * the person likes the page — the theme and the column's width. A place is
 * where a block begins in the file (book.h). The file is found by its name
 * on the whole volume, whatever the Use Context, and made by the first save;
 * a file being read is never written. Its text is one fact a line:
 *
 *     theme sepia
 *     width 72
 *     place 12 4096        the file, and where it was left
 *     mark 12 120          the file, and a bookmark in it
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "box/file.h"

namespace read {

class Memory {
public:
    /* What the volume holds; nothing yet when there is no read.marks. A
     * refusal leaves the memory empty and says why. */
    std::optional<std::string> Load();
    /* Written when anything changed; a refusal says why. */
    std::optional<std::string> Save();

    std::optional<std::size_t> Place(file_id_t File) const;
    void                       SetPlace(file_id_t File, std::size_t Source);
    /* A book's bookmarks, first in the file first. */
    std::vector<std::size_t>   Marks(file_id_t File) const;
    /* A bookmark put where there is none, taken away where there is one;
     * true when one is there now. */
    bool                       Toggle(file_id_t File, std::size_t Source);
    void                       Unmark(file_id_t File, std::size_t Source);

    const std::string &ThemeName() const noexcept { return Theme; }
    std::int32_t       Width() const noexcept { return Cols; }
    void               SetTheme(std::string_view Name);
    void               SetWidth(std::int32_t Width);

private:
    struct Fact {
        file_id_t   File   = 0;
        std::size_t Source = 0;
        bool        operator==(const Fact &) const = default;
    };

    void Read(std::string_view Text);

    std::vector<Fact> Places;
    std::vector<Fact> Marked;
    std::string       Theme;
    std::int32_t      Cols    = 0;
    file_id_t         Kept    = 0;        /* the file read.marks is; 0 before the first save */
    bool              Changed = false;
};

}  // namespace read

#endif
