#ifndef EDIT_SUGGEST_H
#define EDIT_SUGGEST_H

/* The tags a tags field can take, shown under it: the tags files wear, the
 * most worn first, from the volume's own list of them (box::tagfs::tags) —
 * of the Use Context or of the whole volume, as edit is looking. The word
 * being typed narrows them to those that begin with it, the tags already
 * typed are left out, and Up, Down and Enter take one.
 */

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "painter/tui/stage.h"

#include "paint.h"
#include "volume.h"

namespace edit {

class Suggest {
public:
    /* Passed: the key is not the list's. */
    enum class Outcome { Passed, Stay, Closed, Chosen };

    void Begin(Scope Looking);
    void Close() noexcept { Showing = false; }
    bool Open() const noexcept { return Showing; }

    /* The tags that begin with Word, those in Typed left out. */
    void Narrow(std::string_view Word, const std::vector<std::string> &Typed);

    Outcome            OnKey(const tui::KeyEvent &Key);
    const std::string &Chosen() const noexcept { return Picked; }
    void               Paint(tui::Frame &Out, core::Rect Area) const;

private:
    std::vector<box::tagfs::tag_count> All;
    std::vector<std::size_t>           Shown;
    std::string                        Picked;
    std::string                        Where;     /* ScopeNote of the list asked */
    std::size_t                        Row = 0;
    bool                               Showing = false;
};

}  // namespace edit

#endif
