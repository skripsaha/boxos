/* windows.h — the hub's windows: Painter layers over the console's text, in
 * the look every program's windows share (painter/tui/window.h). One of
 * several chosen — the files a name could mean, the programs living behind —
 * and the line at the top of the screen that says who lives behind. What is
 * asked and said is asking's and news's; here it is only shown. */
#ifndef HUB_WINDOWS_H
#define HUB_WINDOWS_H

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "asking.h"
#include "jobs.h"
#include "look.h"

namespace hub::windows {

// One of Choices chosen in a window titled Title over the console's text: Up
// and Down mark one, Enter takes it, a digit takes that one, Esc takes none.
// Answered as asking answers; nothing when no window can be opened here — no
// display draws one — and the caller asks in words instead.
std::optional<asking::Answer> Choose(std::string_view Title, std::span<const std::string> Choices,
                                     std::size_t &Chosen);

// The line at the top of the screen that says who lives behind, when the
// person set the hub to show it there: shown, changed, or taken away. Said by
// one strand alone, always the same: a layer is the strand's that opened it.
void ShowBehind(const Look &Shown, const std::vector<BehindNow> &Living);

}  // namespace hub::windows

#endif
