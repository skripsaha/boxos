#ifndef READ_LISTS_H
#define READ_LISTS_H

/* A window that is a list: the contents (Ctrl+L) and the bookmarks
 * (Ctrl+Shift+M). Up and Down choose, Enter goes where the chosen line leads,
 * Delete takes a bookmark away, Esc shuts it. */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "painter/tui/stage.h"

#include "kin/look.h"

namespace read {

using namespace kin;

struct Entry {
    std::string  Text;
    std::string  Note;           /* on the right: its page */
    std::int32_t Indent = 0;
    std::size_t  Target = 0;     /* what choosing it leads to */
    bool         Odd    = false; /* its note in trouble's ink: a bookmark whose words are gone */
};

class List {
public:
    enum class Outcome { Stay, Closed, Chosen, Dropped };

    void Begin(std::string Title, std::string Hint, std::vector<Entry> Entries, std::size_t Chosen,
               bool Droppable, core::Size Screen);
    Outcome      OnKey(const tui::KeyEvent &Key);
    const Entry &Picked() const noexcept { return Lines[Row]; }
    bool         Empty() const noexcept { return Lines.empty(); }
    void         Paint(tui::Frame &Out) const;

private:
    core::Rect  Area() const noexcept;
    std::size_t Rows() const noexcept;
    void        Follow() noexcept;

    std::string        Title;
    std::string        Hint;
    std::vector<Entry> Lines;
    bool               Droppable = false;
    core::Size         Screen;
    std::size_t        Row   = 0;
    std::size_t        First = 0;
};

}  // namespace read

#endif
