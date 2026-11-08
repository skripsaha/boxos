#ifndef READ_LISTS_H
#define READ_LISTS_H

/* A window that is a list: the contents (c), the bookmarks ('), the keys (?).
 * Up and Down choose, Enter goes where the chosen line leads, Delete takes a
 * bookmark away, Esc shuts it. */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "painter/tui/stage.h"

#include "draw.h"

namespace read {

struct Entry {
    std::string  Text;
    std::string  Note;           /* on the right: a page, a key */
    std::int32_t Indent = 0;
    std::size_t  Target = 0;     /* what choosing it leads to: a block */
};

class List {
public:
    enum class Outcome { Stay, Closed, Chosen, Dropped };

    void Begin(std::string Title, std::string Hint, std::vector<Entry> Entries, std::size_t Chosen,
               bool Droppable, core::Size Screen);
    Outcome      OnKey(const tui::KeyEvent &Key);
    const Entry &Picked() const noexcept { return Lines[Row]; }
    bool         Empty() const noexcept { return Lines.empty(); }
    void         Paint(tui::Frame &Out, const Theme &Look) const;

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
