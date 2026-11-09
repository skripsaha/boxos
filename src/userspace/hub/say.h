/* say.h — the hub's one road to the screen.
 *
 * Everything the hub says goes through boxlib's print, in the order it is
 * said and in the colour set when it is said. C's printf in a C++ program is
 * the runtime's stdout, gathered and handed on later: a second road, on which
 * what is said would come out after what was said on the first, and in
 * whatever colour stood when it was handed on.
 *
 * A refusal is said in words, as the one list of BoxOS errors words it
 * (box/error.h, box::error): "tagfs read only", never a bare number. */
#ifndef HUB_SAY_H
#define HUB_SAY_H

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "box/color.h"
#include "box/cxx/error.h"
#include "box/error.h"
#include "box/print.h"

namespace hub {

inline void Say(std::string_view Text)
{
    print_bytes(Text.data(), Text.size());
}

template <typename... Values>
void Say(std::format_string<Values...> Format, Values &&...Said)
{
    Say(std::string_view(std::format(Format, std::forward<Values>(Said)...)));
}

// Said in Ink, and the console's own colour put back after it.
inline void SayIn(Color Ink, std::string_view Text)
{
    set_color(Ink);
    Say(Text);
    set_color(COLOR_DEFAULT);
}

template <typename... Values>
void SayIn(Color Ink, std::format_string<Values...> Format, Values &&...Said)
{
    SayIn(Ink, std::string_view(std::format(Format, std::forward<Values>(Said)...)));
}

// What a refusal is, in words: Rc a negative -error_t, as the calls answer.
inline std::string_view Reason(int Rc) noexcept
{
    return box::error(box_errno_of(Rc)).message();
}

// The same of an error_t itself, as the calls that fill one in hand it over.
inline std::string_view Reason(error_t Code) noexcept
{
    return box::error(Code).message();
}

}  // namespace hub

#endif
