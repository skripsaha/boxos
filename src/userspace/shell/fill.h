#ifndef FILL_H
#define FILL_H

#include "box/defs.h"
#include "box/types.h"

/* Fill `program`'s Form from `line` (the words after its name, as typed),
 * asking the person only what the filler cannot settle, and start it. `who`
 * is the name the person used; it is word 0 of the program's Luggage and
 * what every sentence begins with. A program made here whose files changed
 * since is made again first (remake.h). Waits until the program is gone and
 * keeps what it gave as `them`. Anything wrong is said here; nothing starts
 * then. */
void FillAndRun(file_id_t program, const char *who, const char *line);

/* Start `program` on the one file `thing` — its Form filled from `it`, that
 * file, asking nobody — and wait until it is gone. True when it ended with 0;
 * *given is then the first file it gave back, 0 when it gave none. The
 * person's `them` is neither read nor changed. Anything wrong is said here. */
bool FillAndRunOn(file_id_t program, const char *who, file_id_t thing, file_id_t *given);

#endif /* FILL_H */
