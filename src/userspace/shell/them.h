#ifndef THEM_H
#define THEM_H

#include "box/defs.h"
#include "box/types.h"
#include "box/form.h"

/* `them` and `it`: the files the last command that gives files gave back.
 * Kept for the session. */

/* Take over `ids` (a heap array, or NULL for none) as what was given; the
 * files held before are let go. */
void ThemKeep(file_id_t *ids, size_t count);

/* Show a lookup the files kept, for the filler to read `them` from. */
void ThemShow(BoxFormLookup *lookup);

/* The files kept; *known is false until a command has given files. */
const file_id_t *ThemFiles(size_t *count, bool *known);

#endif /* THEM_H */
