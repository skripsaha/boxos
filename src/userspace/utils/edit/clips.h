#ifndef EDIT_CLIPS_H
#define EDIT_CLIPS_H

/* The clipboard in edit: what Ctrl+C and Ctrl+X put, what Ctrl+V takes, and
 * the window Ctrl+Shift+V opens on the machine's list. Of that list edit
 * keeps one thing of its own: which of the clips it put are whole lines, so
 * that they go back as lines of their own above the caret's. A number never
 * names another clip, so the number is all there is to keep.
 *
 * The window lists every clip, newest first: how long ago it was put, its
 * first line, its length. A clip with takes left — a password — shows how
 * many and nothing of itself, since a look would spend one. The window opens
 * on the clip before the newest, which Ctrl+V gives already; and when nothing
 * has been put since the last clip pasted from it, on the clip put after
 * that one — clips copied one after another are pasted one after another.
 * What is typed narrows the list to the clips whose beginning holds it.
 */

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "kin/clipboard.h"
#include "kin/field.h"
#include "kin/look.h"

#include "tab.h"

namespace edit {

using namespace kin;

class Clips {
public:
    enum class Outcome { Stay, Closed, Chosen };

    /* What Ctrl+C or Ctrl+X took, as the newest clip; a refusal says why. */
    std::optional<std::string>                      Put(const Clip &Took);
    /* The newest clip as edit pastes it; nothing when the list is empty. */
    std::expected<std::optional<Clip>, std::string> Newest();

    void        Begin(core::Size Screen);
    /* With MayPaste false — the tab's safety is on — Enter shuts the window
     * and takes nothing, since a take may spend a clip. */
    Outcome     OnKey(const tui::KeyEvent &Key, bool MayPaste, std::string &Said);
    const Clip &Chosen() const noexcept { return Picked; }
    void        Paint(tui::Frame &Out) const;

private:
    struct Entry {
        box::clip   Facts;
        std::string Head;     /* its first bytes; none of a clip with takes left */
        std::string Folded;   /* the head in small letters, to match against */
    };

    Clip        AsPasted(std::uint64_t Number, std::string Bytes) const;
    void        Load();
    void        Narrow();
    void        Drop(std::string &Said);
    void        Follow() noexcept;
    core::Rect  Area() const noexcept;
    std::size_t ListRows() const noexcept;
    void        PaintRow(tui::Frame &Out, core::Rect Inside, std::int32_t Y, std::size_t Index) const;

    std::vector<std::uint64_t> WholeLines;      /* the clips this edit put that are whole lines */
    std::uint64_t              LastPasted = 0;  /* the clip last pasted from the window */
    std::uint64_t              NewestThen = 0;  /* and the newest clip when it was */

    core::Size               Screen;
    Field                    Filter{"find"};
    std::vector<Entry>       Entries;
    std::vector<std::size_t> Shown;
    std::string              Trouble;
    std::uint64_t            OpenedUs = 0;      /* when the list was read, as ages are told from */
    std::size_t              Row      = 0;
    std::size_t              First    = 0;
    Clip                     Picked;
};

}  // namespace edit

#endif
