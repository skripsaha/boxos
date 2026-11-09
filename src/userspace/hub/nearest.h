/* nearest.h — the names close to a command that was not found.
 *
 * Closeness is the edit distance with a swap of two neighbours counted as one
 * step: at most 2, and at most 1 for a word of three letters or fewer, which
 * is near to everything otherwise. Only the closest names are kept, three at
 * most. A program's name is its file's name up to the last dot, as the volume
 * stamps it. */
#ifndef HUB_NEAREST_H
#define HUB_NEAREST_H

#include <optional>
#include <string>
#include <string_view>

namespace hub::nearest {

// "did you mean files?" — the built-ins and the programs on the volume close
// to Typed; nothing when none is close.
std::optional<std::string> For(std::string_view Typed);

}  // namespace hub::nearest

#endif
