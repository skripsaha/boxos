#ifndef BOX_CLIPBOARD_H
#define BOX_CLIPBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "boxos_clipboard.h"

/*
 * Clipboard — the machine's list of what was copied.
 *
 * One list, kept by the kernel. A program puts bytes in and is given a
 * number no other clip will ever have; any program lists the clips, newest
 * first, and takes the bytes of any of them by number. The list never reads
 * the bytes and never lets a clip go on its own: a clip leaves when a program
 * drops it, when the list is cleared, or when its own terms end it. The list
 * is empty when the machine starts. What a clip's bytes are — text, a
 * picture, anything — and which clip to take is the program's to decide.
 *
 * The keys every program says the same way:
 *   Ctrl+C, Ctrl+X   copy, cut — the selection is put
 *   Ctrl+V           paste the newest clip
 *   Ctrl+Shift+V     paste one chosen from the list
 */

/* A clip's terms. Takes: how many whole takes it stands before it goes, 0
 * for no such term — such a clip is taken only whole. WhileAlive: it goes
 * when the program that put it ends. A password is put { 1, true }. */
typedef struct {
    uint32_t Takes;
    bool     WhileAlive;
} ClipTerms;

/* Put `size` bytes, at least one; `terms` may be NULL for none. Returns the
 * clip's number, or a negative -error_t: -ERR_NO_MEMORY when the machine has
 * no room for it — nothing in the list is let go to make room. */
int64_t clipboard_put(const void *bytes, size_t size, const ClipTerms *terms);

/* A page of the list: up to `room` clips into `out`, newest first, from the
 * newest numbered `from` or below (0: the newest of all). `page` receives how
 * many came, the number to ask from for the next page (0: none left), and
 * how many clips and bytes the list holds now. Returns 0 or a negative
 * -error_t. */
int clipboard_list(uint64_t from, ClipboardRecord *out, size_t room, ClipboardPage *page);

/* The bytes of clip `number` from `offset` on, as many as `room` holds.
 * Returns how many came, or a negative -error_t: -ERR_OBJECT_NOT_FOUND when
 * no clip holds the number — never one, or gone; -ERR_BUFFER_TOO_SMALL for a
 * clip with takes left asked for less than all of it: such a clip is taken
 * only whole, from 0, and the refusal spends nothing. */
int64_t clipboard_take(uint64_t number, uint64_t offset, void *buf, size_t room);

/* Clip `number` out of the list. Returns 0 or a negative -error_t. */
int clipboard_drop(uint64_t number);

/* Every clip out of the list. Returns how many went, or a negative -error_t. */
int64_t clipboard_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* BOX_CLIPBOARD_H */
