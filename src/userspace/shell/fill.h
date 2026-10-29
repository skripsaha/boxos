#ifndef FILL_H
#define FILL_H

#include "box/defs.h"
#include "box/types.h"
#include "box/form.h"

/* The shell's view for a filler: the volume, `them`, and the names of the
 * running Itinerary's fields. */
void FillLookup(BoxFormLookup *lookup);

/* Form filled from `line` over `lookup`, asking the person only what the
 * filler cannot settle: true with *filler done (the caller ends it), or false
 * with nothing held, anything wrong said. `lookup` outlives the filler. */
bool FillForm(const BoxForm *form, BoxFormLookup *lookup, const char *who, const char *line,
              BoxFormFiller **filler);

/* Fill `program`'s Form from `line` (the words after its name, as typed),
 * asking the person only what the filler cannot settle, and start it. `who`
 * is the name the person used; it is word 0 of the program's Luggage and
 * what every sentence begins with. A program made here whose files changed
 * since is made again first (remake.h). Waits until the program is gone and
 * keeps what it gave as `them`. Anything wrong is said here; nothing starts
 * then. True when the program went through: it started and ended with 0. */
bool FillAndRun(file_id_t program, const char *who, const char *line);

/* Start `program` on the one file `thing` — its Form filled from `it`, that
 * file, asking nobody — and wait until it is gone. True when it ended with 0;
 * *given is then the first file it gave back, 0 when it gave none. The
 * person's `them` is neither read nor changed. Anything wrong is said here. */
bool FillAndRunOn(file_id_t program, const char *who, file_id_t thing, file_id_t *given);

#endif /* FILL_H */
