#ifndef BOX_LUGGAGE_H
#define BOX_LUGGAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"

/*
 * Luggage — what the spawner said to this program at boarding.
 *
 * When the person types `say hello   world`, the kernel puts that line, as
 * typed, into the new program's cabin before its first instruction runs. It
 * is not sent and not received; it is simply there, in the CabinInfo page or
 * in the buffer heap when it is too long for the page, with no ceiling but
 * memory. Word 0 is the program's own name as it was typed. A program started
 * with nothing (autostart, a bare proc_exec) has an empty luggage.
 *
 * The words are cut the way the shell cuts its own line: blanks separate,
 * "double quotes" group, and the cut happens once, on the first ask.
 */

typedef struct {
    const char *bytes;    /* the line as typed, NOT NUL-terminated; NULL when there is none */
    uint32_t    length;
} Luggage;

/* The whole line, verbatim, straight from the cabin page. */
Luggage luggage(void);

/* How many words the line has. */
uint32_t luggage_word_count(void);

/* The i-th word as a NUL-terminated string (quotes removed), or NULL past the
 * last one. Word 0 is the program's name. The string lives as long as the
 * cabin. */
const char *luggage_word(uint32_t index);

/* The line from the start of word `from` to its end, as typed — spaces and
 * quotes kept — NUL-terminated; "" past the last word. What `say` prints. */
const char *luggage_tail(uint32_t from);

/* Cut `line` into words in place, the way the shell and the luggage do:
 * blanks separate, "double quotes" group, separators become NULs. Writes up
 * to `max` word pointers into `words` and returns how many words the line
 * has, counting those past `max` as well, so a caller can size an array. */
uint32_t luggage_cut(char *line, char **words, uint32_t max);

#ifdef __cplusplus
}
#endif

#endif /* BOX_LUGGAGE_H */
