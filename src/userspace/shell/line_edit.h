#ifndef LINE_EDIT_H
#define LINE_EDIT_H

#include "box/defs.h"

/* =========================================================================
 * LineEdit — line editor for the BoxOS shell.
 *
 * Today this is a thin wrapper over readline() (display daemon →
 * kb_readline → kernel blocking read). Future expansion to per-char
 * editing requires display-daemon protocol support for non-blocking
 * key events — until then, blocking readline is the correct primitive.
 *
 * History is captured on every accepted line for future up/down recall
 * once interactive editing lands.
 * ========================================================================= */

#define LINE_CAPACITY       512     /* max single line (generous for a shell) */
#define HISTORY_DEPTH       32
#define HISTORY_LINE_MAX    256     /* max stored per history entry */

/* Return codes from LineEditRead */
#define LINE_OK             0
#define LINE_EMPTY          1
#define LINE_ERROR          (-1)

typedef struct {
    char buf[LINE_CAPACITY];
    int  len;
    int  cursor;        /* byte position within buf */
} LineBuffer;

typedef struct {
    char lines[HISTORY_DEPTH][HISTORY_LINE_MAX];
    int  count;         /* total stored entries (0..HISTORY_DEPTH) */
    int  write_pos;     /* next slot to write (circular) */
    int  browse_pos;    /* current browsing offset during up/down (-1 = not browsing) */
} LineHistory;

typedef struct {
    LineBuffer  line;
    LineHistory history;
    int         prompt_len;     /* visual columns occupied by prompt */
} LineEditState;

void LineEditInit(LineEditState *st);

/* Read one line interactively.  Renders prompt, handles keys, returns
 * LINE_OK / LINE_EMPTY / LINE_EXIT_REQUEST / LINE_ERROR.
 * On LINE_OK, out[0..max-1] contains the NUL-terminated input. */
int  LineEditRead(LineEditState *st, const char *prompt,
                  char *out, int max_len);

#endif /* LINE_EDIT_H */
