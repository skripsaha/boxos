/* notes.h — a note and its file. A note is a file of the volume wearing
 * scratchpad's own word "scratchpad", its text the file's bytes, its colors
 * two more of its own words beside it: paper:RRGGBB and ink:RRGGBB. So every
 * other program sees a note as the text it is, and the person sees those tags
 * in info, under the program's name. A note not yet saved has no file;
 * the first save makes it, named as the person said or by the words its first
 * line begins with. */
#ifndef SCRATCHPAD_NOTES_H
#define SCRATCHPAD_NOTES_H

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "box/types.h"

#include "look.h"
#include "text.h"

namespace scratchpad {

class Note {
public:
    // The note that file holds. A file too large for a note, or one that could
    // not be read, is a sentence for the person.
    static std::expected<Note, std::string> Read(file_id_t File, const Look &Shown);
    // A note with nothing in it yet; Name may be empty.
    static Note Blank(std::string Name, const Look &Shown);

    // The note on the volume, made if it has no file: the text and the colors.
    // Nothing is done for a note that has nothing new. A refusal is a sentence.
    std::optional<std::string> Save();
    // The file in the trash, where it can be taken back from.
    std::optional<std::string> Trash();

    // The colors of the note, chosen by the person.
    void Repaint(Sheet Now) noexcept;
    // A note whose colors are only the settings' follows them when they change.
    void Follow(const Look &Shown) noexcept;

    bool                Unsaved() const noexcept;
    file_id_t           File() const noexcept { return Held; }
    const std::string  &Name() const noexcept { return Called; }
    const Sheet        &Paints() const noexcept { return Colors; }

    NoteText    Text;
    std::size_t Top = 0;    // the first row of the text shown

private:
    Note() = default;

    std::optional<std::string> Retag();
    std::optional<std::string> Write();

    file_id_t   Held    = 0;
    std::string Called;
    Sheet       Colors{};
    bool        Stored  = false;    // the file wears the colors
    bool        Worn    = false;    // the file wears "scratchpad"
    bool        Changed = false;    // the colors are not what the file wears
    bool        Follows = false;
};

// Every note on the volume, but those in the trash: the files wearing
// scratchpad's own word, which is asked of the whole volume — the Use Context
// is the person's.
std::expected<std::vector<file_id_t>, std::string> AllNotes();

}  // namespace scratchpad

#endif
