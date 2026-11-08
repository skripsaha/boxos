#ifndef EDIT_FIELD_H
#define EDIT_FIELD_H

/* One line typed into a window: what to find, a line's number, a name, tags.
 * The same keys move in it everywhere — Ctrl+V among them, which puts the
 * newest clip's first line in — and it draws itself the same way in every
 * window, its caret placed in the frame when the window is listening to it.
 * Its caret moves a whole character at a time, however many bytes the
 * character is. */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "painter/tui/frame.h"
#include "painter/tui/stage.h"

namespace edit {

class Field {
public:
    explicit Field(std::string_view Label = {}) : Label(Label) {}

    const std::string &Value() const noexcept { return Words; }
    void               Set(std::string_view Value);
    /* A value put in for the person to take or leave: it is shown marked, the
     * first character typed replaces it, and a key that moves keeps it. */
    void               Offer(std::string_view Value);
    void               Clear() noexcept;

    /* A character, Backspace, Delete, Left, Right, Home or End: true when Key
     * was one of them, whether or not it changed anything. */
    bool OnKey(const painter::tui::KeyEvent &Key);

    /* The label, then the value, on one row from At, Width cells wide. */
    void Paint(painter::tui::Frame &Out, painter::core::Point At, std::int32_t Width, bool Listening) const;

private:
    void Put(std::string_view Bytes);

    std::string Label;
    std::string Words;
    std::size_t Caret   = 0;
    bool        Offered = false;
};

}  // namespace edit

#endif
