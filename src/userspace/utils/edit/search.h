#ifndef EDIT_SEARCH_H
#define EDIT_SEARCH_H

/* Finding and replacing: Ctrl+F and Ctrl+R.
 *
 * The caret goes to the first match while the words are typed, and every
 * match on the glass is marked. Letters of both sizes are the same unless the
 * words hold a capital. A replace of every match is one undo.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "field.h"
#include "paint.h"
#include "tab.h"

namespace edit {

class Search {
public:
    enum class Outcome { Stay, Closed };

    void    Begin(Tab &Active, bool Swap, core::Size Screen);
    Outcome OnKey(const tui::KeyEvent &Key, Tab &Active, std::size_t Rows, std::string &Said);
    void    Paint(tui::Frame &Out, Tab &Active);

    /* Where the words stand in the tab, in order, and how many bytes each match is. */
    const std::vector<Position> &Matches(const Tab &Active);
    std::size_t                  NeedleBytes() const noexcept { return What.Value().size(); }

private:
    bool        Folded() const noexcept;
    core::Rect  Area() const noexcept;
    bool        AtMatch(const Tab &Active);
    void        Seek(Tab &Active, Position From, bool Forward, std::size_t Rows);
    void        SwapOne(Tab &Active, std::size_t Rows, std::string &Said);
    void        SwapAll(Tab &Active, std::string &Said);

    Field                 What{"find"};
    Field                 With{"with"};
    bool                  Swapping = false;
    bool                  OnWith   = false;
    Position              Origin;
    core::Size            Screen;

    std::vector<Position> Found;
    const Tab            *FoundIn = nullptr;
    std::uint64_t         FoundVersion = 0;
    std::string           FoundNeedle;
};

}  // namespace edit

#endif
