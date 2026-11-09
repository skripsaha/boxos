/* remake.h — a program made here starts as what its files make now.
 *
 * Before a program starts: when it carries a Recipe and a file it was made
 * from changed since, the tool the Recipe names makes it again — the program
 * handed to it as `it` — and the program made is the one that starts. When it
 * cannot be made, nothing starts: the person asked for the program their
 * files make, not an older one. A file that is gone makes nothing: it starts
 * as it was made, and the person is told. No Recipe — the system's own
 * programs, built on the host — starts as it is. A program made again takes
 * its own place, in `them` too. */
#ifndef HUB_REMAKE_H
#define HUB_REMAKE_H

#include <string_view>

#include "box/types.h"

namespace hub {

struct Hub;

namespace remake {

// True: start Program — the one made again, when it was. False: not started,
// and why was said. Who begins every sentence.
bool WhenChanged(Hub &Self, file_id_t &Program, std::string_view Who);

}  // namespace remake
}  // namespace hub

#endif
