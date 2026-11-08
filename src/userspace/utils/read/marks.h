#ifndef READ_MARKS_H
#define READ_MARKS_H

/* The bookmarks read keeps from one reading to the next, in a file of its own
 * on the volume, read.marks: those the person puts, and the one read puts
 * where a book was left, so the book opens there again.
 *
 * A bookmark is where a paragraph begins in the file, and the words it begins
 * with. When the file is changed and other words stand there, the bookmark
 * goes where its words went — to the place nearest the old one where they
 * stand; when they are in the file no more, it stays where it was and is lost,
 * and is shown so. A bookmark of a file the volume no longer has is let go.
 *
 * read.marks is found by its name on the whole volume, whatever the Use
 * Context, and made by the first save; a book read is never written. Its text
 * is one bookmark a line — what it is, the file, the place, and the words:
 *
 *     place 12 4096 ## The clipboard
 *     mark 12 120 BoxOS keeps no folders. A file is
 */

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "box/file.h"

namespace read {

struct Mark {
    std::size_t Source = 0;      /* where its paragraph begins in the file */
    bool        Lost   = false;  /* its words are in the file no more */
};

class Memory {
public:
    /* What the volume holds; nothing yet when there is no read.marks. A
     * refusal leaves the memory empty and says why. */
    std::optional<std::string> Load();
    /* Written when anything changed; a refusal says why. */
    std::optional<std::string> Save();

    /* The bookmarks of File found again in its bytes as they are now. */
    void              Settle(file_id_t File, std::string_view Bytes);
    std::optional<Mark> Place(file_id_t File) const;
    void              SetPlace(file_id_t File, std::size_t Source, std::string_view Bytes);
    /* A book's bookmarks, first in the file first. */
    std::vector<Mark> Marks(file_id_t File) const;
    void              Put(file_id_t File, std::size_t Source, std::string_view Bytes);
    void              Unmark(file_id_t File, std::size_t Source);

private:
    struct Fact {
        bool        Left   = false;  /* where the book was left, not a bookmark the person put */
        file_id_t   File   = 0;
        std::size_t Source = 0;
        std::string Words;
        bool        Lost   = false;
    };

    void Read(std::string_view Text);
    void LetGoOfTheGone();

    std::vector<Fact> Facts;
    file_id_t         Kept    = 0;        /* the file read.marks is; 0 before the first save */
    bool              Changed = false;
};

}  // namespace read

#endif
