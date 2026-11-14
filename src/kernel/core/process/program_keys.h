#ifndef PROGRAM_KEYS_H
#define PROGRAM_KEYS_H

#include "ktypes.h"

/*
 * The keys a cabin starts with — what a process started from a file wears
 * before its box is read.
 *
 * A file's tags are its user's labels and decide nothing about the process
 * made of it. What a program is and may do comes from the program: its box
 * asks for authority (the asks part, box_reader.h; worn by
 * process_load_binary), and the system gives `system` to the file whose
 * record says it is the system's (TAGFS_FILE_ROLE_SYSTEM, set by the image
 * builder and by nothing at run time). Here is the rest: every program is an
 * app (`app` — storage, the Touch tags, the operations everyone may use), and
 * is known by the name of its file, so another program can find it running
 * (the stem of `edit.box` is `edit`).
 */

/* The comma list of keys for a program of the file called `filename`, whose
 * record carries `flags`, in a heap string the caller kfree's: `app`, the
 * stem of the name unless it is spelled as no key or is one the system keeps
 * for itself, and `system` for a file of the system's role. NULL when there is
 * no memory. */
char *ProgramKeys(const char *filename, uint32_t flags);

#endif /* PROGRAM_KEYS_H */
