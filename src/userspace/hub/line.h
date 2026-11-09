/* line.h — one line run as if typed: a built-in with its words, or a program —
 * or an Itinerary — by its name, the rest of the line its own. What the
 * prompt and an Itinerary's steps both run. */
#ifndef HUB_LINE_H
#define HUB_LINE_H

#include <string_view>

namespace hub {

struct Hub;

// True when the line went through; when it did not, why was said.
bool RunLine(Hub &Self, std::string_view Line);

}  // namespace hub

#endif
