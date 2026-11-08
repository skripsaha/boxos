#ifndef EDIT_PICKER_H
#define EDIT_PICKER_H

/* The window a file is chosen in: Ctrl+N, and edit started with nothing to
 * open. It lists the files of the Use Context — the whole volume when the
 * person has said none, or when Tab asks for it — and every word typed
 * narrows the list: a word is a piece of a file's name or the beginning of
 * one of its tags. A name that no file has is offered as a new file. */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "box/file.h"

#include "field.h"
#include "paint.h"

namespace edit {

class Picker {
public:
    enum class Outcome { Stay, Closed, Chosen };

    /* A file of the volume, or a new one when File is 0. */
    struct Choice {
        file_id_t   File = 0;
        std::string Name;
    };

    void           Begin(core::Size Screen, std::vector<file_id_t> Open);
    Outcome        OnKey(const tui::KeyEvent &Key);
    const Choice  &Chosen() const noexcept { return Picked; }
    void           Paint(tui::Frame &Out) const;

private:
    struct Entry {
        file_id_t                File = 0;
        std::string              Name;
        std::string              Folded;      /* the name in small letters, to match against */
        std::vector<std::string> Tags;        /* as shown: not the one for its name */
        std::uint64_t            Size = 0;
    };

    void        Load();
    void        Narrow();
    bool        Fits(const Entry &One, const std::vector<std::string> &Words) const;
    bool        OffersNew() const noexcept { return !NewName.empty(); }
    std::size_t RowCount() const noexcept { return Shown.size() + (OffersNew() ? 1 : 0); }
    core::Rect  Area() const noexcept;
    std::size_t ListRows() const noexcept;
    void        PaintRow(tui::Frame &Out, core::Rect Inside, std::int32_t Y, std::size_t Index) const;

    core::Size               Screen;
    Field                    Filter{"find"};
    std::vector<Entry>       Entries;
    std::vector<std::size_t> Shown;
    std::string              NewName;
    std::vector<file_id_t>   AlreadyOpen;
    std::string              Context;
    std::string              Trouble;
    bool                     Everywhere = false;
    std::size_t              Row = 0;
    std::size_t              First = 0;
    Choice                   Picked;
};

}  // namespace edit

#endif
