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

/* A file kept in place of another: a program made again by its Recipe
 * takes its own place, so `them` holds the new one where it held the old. */
void ThemReplace(file_id_t old, file_id_t made);

/* The kept files taken aside — `them` is nothing given after it — and put
 * back: what an Itinerary does with the person's own `them` while it runs. */
typedef struct {
    file_id_t *Files;
    size_t     Count;
    bool       Known;
} ThemSaved;

void ThemSave(ThemSaved *saved);
/* The saved files are `them` again; what was kept since is let go. */
void ThemRestore(ThemSaved *saved);
/* The saved files are let go; what is kept now stays. */
void ThemRelease(ThemSaved *saved);

/* Show a lookup the files kept, for the filler to read `them` from. */
void ThemShow(BoxFormLookup *lookup);

/* The files kept; *known is false until a command has given files. */
const file_id_t *ThemFiles(size_t *count, bool *known);

#endif /* THEM_H */
