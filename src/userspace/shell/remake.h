#ifndef REMAKE_H
#define REMAKE_H

#include "box/defs.h"
#include "box/types.h"

/* Before `program` starts: when it carries a Recipe and a file it was made
 * from has changed since, the tool the Recipe names makes it again, in the
 * Use Context it was made in, and *program becomes the program made. A file
 * that is gone makes nothing: it starts as it was made, and the person is
 * told. True: start *program. False: not started — it had to be made again
 * and could not be; said why. `who` begins every sentence. */
bool RemakeWhenChanged(file_id_t *program, const char *who);

#endif /* REMAKE_H */
