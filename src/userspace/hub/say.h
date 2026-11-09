/* say.h — the hub's one road to the screen.
 *
 * Everything the hub says goes through boxlib's print, in the order it is
 * said and in the colour set when it is said. C's printf in a C++ program is
 * the runtime's stdout, gathered and handed on later: a second road, on which
 * what is said would come out after what was said on the first, and in
 * whatever colour stood when it was handed on. */
#ifndef HUB_SAY_H
#define HUB_SAY_H

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "box/print.h"

namespace hub {

inline void Say(std::string_view Text)
{
    print_bytes(Text.data(), Text.size());
}

template <typename... Values>
void Say(std::format_string<Values...> Format, Values &&...Said)
{
    const std::string Text = std::format(Format, std::forward<Values>(Said)...);
    print_bytes(Text.data(), Text.size());
}

}  // namespace hub

#endif
