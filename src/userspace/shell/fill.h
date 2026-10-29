#ifndef FILL_H
#define FILL_H

#include "box/defs.h"
#include "box/types.h"

/* Fill `program`'s Form from `line` (the words after its name, as typed),
 * asking the person only what the filler cannot settle, and start it. `who`
 * is the name the person used; it is word 0 of the program's Luggage and
 * what every sentence begins with. Waits until the program is gone and keeps
 * what it gave as `them`. Anything wrong is said here; nothing starts then. */
void FillAndRun(file_id_t program, const char *who, const char *line);

#endif /* FILL_H */
