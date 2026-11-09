#ifndef RISE_H
#define RISE_H

#include "ktypes.h"

/*
 * Rise — a program that falls, raised again by the system.
 *
 * A program whose box says `rise` (its life part, box_reader.h) and that the
 * system started — the autostart of a volume, or a rise before it — is watched
 * from its start. When it falls (proc_exit.h: PROC_EXIT_CRASHED, or
 * PROC_EXIT_STACK_OVERFLOW) it is raised again from its file the way the
 * autostart starts it — the file still wearing autostart and a program's tag,
 * and not in the trash — after a pause: 1 s, twice the last for every fall in
 * a row, 16 s at most; a program that ran a minute before it fell starts over
 * at 1 s. Three falls in a row, each within 10 s of its start, and it is let
 * be: raised again it would only fall again. An end that is no fall — an
 * exit, a kill — is its own, and it is not raised. Every fall, raise and
 * letting be is said in the journal. A program a person started is the
 * person's: the system never raises it.
 *
 * The raise comes through one door: the touch queue calls at the tick it is
 * due (a node of rise's own, armed at the earliest), the call knocks, and a
 * K-Core opens — starting a program reads the volume, which no interrupt may.
 */

struct process_t;

/* Before the first program is started: the door the raises come through. */
void RiseInit(void);

/* `proc`, made by the system from the file `file` called `name` and not yet
 * run, watched when its box says rise. Its CabinInfo says how many times it
 * was raised. */
void RiseWatch(struct process_t *proc, uint64_t file, const char *name);

/* The end of the program (pid, generation) — the disposition its main strand
 * ended with (proc_exit.h): a watched program's fall raises it again, any
 * other end lets it go. Takes rise's lock alone and allocates nothing:
 * callable wherever a program ends. */
void RiseProgramEnded(uint32_t pid, uint32_t generation, int32_t disposition);

#endif /* RISE_H */
