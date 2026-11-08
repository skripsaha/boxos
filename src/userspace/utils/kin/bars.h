#ifndef KIN_BARS_H
#define KIN_BARS_H

/* The two bars edit and read stand between: the tabs across the top, and the
 * status line at the foot — on its right what the program has just said or
 * the way to its keys, on its left what the program says of the tab in front.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "look.h"

namespace kin {

/* A tab as the bar shows it. */
struct TabFace {
    std::string_view Name;
    bool             Dirty  = false;   /* holds changes not saved: a dot after its name */
    bool             Warned = false;   /* its number in trouble's ink: the tab may not be written */
};

/* A tab is " N name " and " •" more while it holds changes not saved. When
 * they do not all fit, the names are shortened alike; when even the shortest
 * names do not, the tabs about the one in front are shown and arrows say
 * there are more. */
void PaintTabs(tui::Frame &Out, std::int32_t Width, std::span<const TabFace> Tabs, std::size_t Current);

/* The status line, drawn as it is made: Said on the right, in good ink, or in
 * trouble's when Trouble; Hint there when nothing was said. What is put from
 * the left stops short of it. */
class StatusLine {
public:
    StatusLine(tui::Frame &Out, core::Size Screen, std::string_view Said, bool Trouble,
               std::string_view Hint = "Ctrl+? keys");

    void Put(std::string_view Text, core::Color Ink, core::Color Under = BarPaper);
    /* A word on paper of its own: " WORD ", for a state the tab is in. */
    void Badge(std::string_view Word, core::Color Under);
    /* The cells left before the right of the line. */
    std::int32_t Room() const noexcept { return End > X ? End - X : 0; }

private:
    tui::Frame  &Out;
    std::int32_t Y;
    std::int32_t X = 1;
    std::int32_t End;
};

}  // namespace kin

#endif
