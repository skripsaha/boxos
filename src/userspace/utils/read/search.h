#ifndef READ_SEARCH_H
#define READ_SEARCH_H

/* Finding, Ctrl+F, as edit finds: a window at the top right, the words in it
 * looked for while they are typed — the reading line goes to the first place
 * they stand from where it stood, and back there when they stand nowhere —
 * and every place they stand lit while the window is open. Letters of both
 * sizes are the same unless the words hold a capital. Enter and Down go to
 * the next place, Shift+Enter and Up to the one before; Esc shuts the window
 * and leaves the reading line where it is. The words are looked for as the
 * page shows them: in prose, without the marks Markdown writes around them.
 */

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "kin/field.h"
#include "kin/look.h"

#include "tab.h"

namespace read {

using namespace kin;

class Search {
public:
    enum class Outcome { Stay, Closed };

    /* Bytes [From, To) of a block's text. */
    struct Match {
        std::size_t Block = 0;
        std::size_t From  = 0;
        std::size_t To    = 0;
    };

    /* The rows under the tabs the window stands over. */
    static constexpr std::size_t Covered = 3;

    void    Begin(Tab &Active, core::Size Screen);
    Outcome OnKey(const tui::KeyEvent &Key, Tab &Active, std::size_t Rows);
    void    Paint(tui::Frame &Out) const;

    /* Every place the words stand in the book in front, in order. */
    const std::vector<Match>  &Matches() const noexcept { return Found; }
    /* The place the reading line was sent to. */
    std::optional<std::size_t> Here() const noexcept { return At; }

private:
    void Refresh(const Tab &Active);
    void Seek(Tab &Active, std::size_t Index, std::size_t Rows);
    /* The first place at or after where the reading line stood. */
    std::size_t FirstFromOrigin() const noexcept;

    Field                      What{"find"};
    core::Size                 Screen;
    std::vector<Match>         Found;
    std::optional<std::size_t> At;
    std::size_t                OriginBlock = 0, OriginByte = 0, OriginTop = 0, OriginReading = 0;
};

}  // namespace read

#endif
