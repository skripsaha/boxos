#ifndef SUGGEST_H
#define SUGGEST_H

#include "box/defs.h"
#include "box/types.h"

/* The names close to a command that was not found — the built-ins and the
 * programs on the volume by their stem — as "did you mean files?" into `out`
 * (up to `size` bytes). False, and `out` untouched, when none is close. */
bool SuggestFor(const char *typed, char *out, size_t size);

#endif /* SUGGEST_H */
