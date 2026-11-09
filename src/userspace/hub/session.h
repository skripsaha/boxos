/* session.h — the hub talking with the person: a prompt, a line, the line run. */
#ifndef HUB_SESSION_H
#define HUB_SESSION_H

#include <string_view>

namespace hub {

struct Hub;

// One line run as if typed: a built-in with its words, a program with the
// line as its Luggage. True when it went through; when it did not, why was
// said.
bool RunLine(Hub &Self, std::string_view Line);

// Prompt after prompt until `exit` is said. False when nobody can type here.
bool Converse(Hub &Self);

}  // namespace hub

#endif
