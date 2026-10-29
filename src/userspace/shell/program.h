#ifndef PROGRAM_H
#define PROGRAM_H

#include "box/defs.h"
#include "box/types.h"

/* A program is a file that is active and wears the tag app or utility: what
 * the kernel starts when it is given a name or a file id. */
bool ProgramIsRunnable(file_id_t id);

/* The file's name, a heap string the caller frees; NULL when it cannot be read. */
char *ProgramNameOf(file_id_t id);

/* How a pick ended. A negative value is an -error_t and nothing was said. */
#define PROGRAM_PICKED     0
#define PROGRAM_SAID       1   /* the person was told why there is no program */

/* The program a command NAME means: the files wearing the tag NAME that are
 * programs. One is taken; several are asked which; none is
 * -ERR_FILE_NOT_FOUND, said by the caller. */
int ProgramPick(const char *name, file_id_t *program);

/* The program a THING typed means: a file here (or else anywhere) by that
 * name, or `it`/`them`, which must be a program. Asked which when it names
 * several; said what is wrong when it names no program. */
int ProgramPickThing(const char *thing, file_id_t *program);

#endif /* PROGRAM_H */
