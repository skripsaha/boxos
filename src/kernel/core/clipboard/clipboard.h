#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include "ktypes.h"
#include "error.h"
#include "boxos_clipboard.h"

typedef struct cabin_t cabin_t;

/*
 * Clipboard — the machine's list of what was copied.
 *
 * One list, the kernel's, so it is there from the first program on and no
 * program's end takes it away. A clip is bytes the list never reads, under a
 * number that grows and is never given twice: a number a program holds names
 * that clip or none. The list keeps every clip until it is dropped, cleared,
 * or its terms end it; nothing is let go to make room — memory that will not
 * hold a new clip refuses the put. What to put, what to take, and what a
 * clip's bytes mean are the programs' to say.
 *
 * HOW IT IS KEPT. The clips hang newest first in one list under one lock, and
 * the lock guards the links and the counts and nothing else: no allocation,
 * no copy, no print and no other lock is ever taken under it. A clip is made
 * whole outside it — every byte copied in — and linked in one step, so nobody
 * sees part of a clip. A taker holds a reference and copies with no lock at
 * all; a clip that leaves is unlinked under the lock, and whoever lets go of
 * it last wipes its bytes and frees them.
 *
 * TERMS. A clip put with takes stands that many whole takes and no more. The
 * take that spends the last one unlinks the clip under the same hold of the
 * lock that found it, before a byte is copied, so of any number of takers
 * exactly that many get it. Such a clip is taken whole or not at all: no part
 * of it leaves on its own, and a look at its beginning cannot spend it. A
 * clip put WHILE_ALIVE goes with the program that put it — the (pid,
 * generation) of the cabin it came from (CabinProgram) — when that cabin is
 * torn down.
 */

/* Where a put's bytes come from: `n` of them, from byte `offset` on, into
 * `into`. */
typedef error_t (*ClipFill)(void *source, uint64_t offset, void *into, uint64_t n);

/* Where a take's bytes go: `n` of them, from byte `offset` of the take on,
 * out of `from`. */
typedef error_t (*ClipPour)(void *sink, uint64_t offset, const void *from, uint64_t n);

/*
 * A clip of `bytes` bytes, filled by `fill` from `source`, at the head of the
 * list; *number its number. takes and flags are its terms (boxos_decks.h);
 * WHILE_ALIVE ties it to the program `putter` is. A fill that fails lets the
 * clip go before anyone sees it, and says why.
 */
error_t ClipboardPut(uint64_t bytes, ClipFill fill, void *source, uint32_t takes, uint32_t flags,
                     const cabin_t *putter, uint64_t *number);

/*
 * Up to `room` clips into `out`, newest first, from the newest numbered
 * `from` or below (0: the newest of all). *next is the number to ask from for
 * the rest, 0 when none is left; *clips and *bytes are what the list holds
 * now. Returns how many it wrote.
 */
uint32_t ClipboardList(uint64_t from, ClipboardRecord *out, uint32_t room, uint64_t *next, uint64_t *clips,
                       uint64_t *bytes);

/*
 * The bytes of clip `number` from `offset` on, no more than `room` of them,
 * poured into `sink`; *given how many. A clip with takes left is taken only
 * whole (ERR_BUFFER_TOO_SMALL otherwise, nothing spent), and the take is
 * spent before it is poured: a sink that refuses the bytes has spent it all
 * the same. ERR_OBJECT_NOT_FOUND when no clip holds the number,
 * ERR_INVALID_ARGUMENT for an offset at or past the clip's end.
 */
error_t ClipboardTake(uint64_t number, uint64_t offset, uint64_t room, ClipPour pour, void *sink,
                      uint64_t *given);

/* Clip `number` out of the list. ERR_OBJECT_NOT_FOUND when no clip holds it. */
error_t ClipboardDrop(uint64_t number);

/* Every clip out of the list; how many went. */
uint64_t ClipboardClear(void);

/* The cabin is being torn down: the clips its program put WHILE_ALIVE go.
 * One load when no clip in the list was put so. */
void ClipboardCleanupCabin(const cabin_t *cabin);

#endif /* CLIPBOARD_H */
