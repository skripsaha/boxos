/* session.h — the hub talking with the person: a prompt, a line, the line
 * kept and run. */
#ifndef HUB_SESSION_H
#define HUB_SESSION_H

namespace hub {

struct Hub;

// Prompt after prompt until `exit` is said. False when nobody can type here.
bool Converse(Hub &Self);

}  // namespace hub

#endif
