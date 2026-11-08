#ifndef READ_KEYMAP_H
#define READ_KEYMAP_H

/* Every key read answers while the page is in front, in one table, heard and
 * told as edit's are (kin/keys.h): the same key does the same thing in both,
 * and what read has that edit has not — the bookmarks, the contents, the
 * column — is said in the same way. The keys a window answers while it is
 * open are that window's own, and its bottom edge names them.
 */

#include <cstdint>
#include <optional>
#include <vector>

#include "kin/keys.h"

namespace read {

using namespace kin;

enum class Command : std::uint8_t {
    Open, CloseTab, Card, GoToTab, NextTab, PriorTab, Scope, Leave,
    Up, Down, PageUp, PageDown, FileStart, FileEnd, NextHeading, PriorHeading, Contents, SelectAll,
    Copy, Mark, Marks, Wider, Narrower, Turn,
    Find, Help,
};

/* Where the help puts a key. */
enum class Part : std::uint8_t { Files, Moving, Reading, Finding };

using Heard = kin::Heard<Command>;

std::optional<Heard> Hear(const painter::tui::KeyEvent &Key) noexcept;
/* The help, written from the table. */
std::vector<KeyLine> HelpLines();

}  // namespace read

#endif
