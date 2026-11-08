#ifndef READ_THEME_H
#define READ_THEME_H

/* The looks a page can have: dark, sepia, light. Each is chosen for a
 * framebuffer and checked against the sixteen colours of a VGA text screen it
 * is folded into there, where its paper, its ink and its reading line are
 * still told apart. */

#include <span>
#include <string_view>

#include "painter/core/color.h"

namespace read {

namespace core = painter::core;

struct Theme {
    std::string_view Name;
    core::Color      Paper;
    core::Color      Ink;
    core::Color      Faint;          /* quotes' bars, rules, page numbers, hints */
    core::Color      Heading;        /* the first two levels */
    core::Color      Subheading;     /* the rest */
    core::Color      Strong;
    core::Color      Emphasis;
    core::Color      Code;
    core::Color      CodePaper;
    core::Color      Link;
    core::Color      ReadingPaper;   /* the line being read */
    core::Color      SelectedPaper;
    core::Color      MatchPaper;
    core::Color      HerePaper;      /* the match the search stands at */
    core::Color      HereInk;
    core::Color      BarPaper;
    core::Color      BarInk;
    core::Color      Accent;         /* a bookmark's sign, what the bars point out */
    core::Color      WindowPaper;
    core::Color      WindowRule;
    core::Color      ChosenPaper;
    core::Color      Trouble;
};

std::span<const Theme> Themes() noexcept;
/* The theme of that name; the first when no theme has it. */
const Theme &ThemeNamed(std::string_view Name) noexcept;
const Theme &ThemeAfter(const Theme &One) noexcept;

}  // namespace read

#endif
