/*
 * line_edit.c — Line editor for BoxOS shell
 *
 * Uses readline() (blocking, through display daemon) for input.
 * Adds command history on top — up/down arrows handled by display's
 * readline when available, history tracked after line submission.
 *
 * This preserves the WORKING input model of the original shell:
 *   readline() → display daemon → kb_readline → kernel blocking read
 *
 * Future: per-character editing with cursor movement requires kernel
 * support for blocking getchar (HW_KEYBOARD_GETCHAR currently returns
 * immediately with 0 if no key). Until then, readline is the correct
 * approach.
 */

#include "line_edit.h"
#include "box/print.h"
#include "box/string.h"

/* =========================================================================
 * History
 * ========================================================================= */

static void HistoryAdd(LineHistory *h, const char *line)
{
    if (!line || line[0] == '\0') return;

    /* Don't duplicate most recent entry */
    if (h->count > 0) {
        int prev = (h->write_pos - 1 + HISTORY_DEPTH) % HISTORY_DEPTH;
        if (strcmp(h->lines[prev], line) == 0) return;
    }

    size_t len = strlen(line);
    if (len >= HISTORY_LINE_MAX) len = HISTORY_LINE_MAX - 1;
    memcpy(h->lines[h->write_pos], line, len);
    h->lines[h->write_pos][len] = '\0';

    h->write_pos = (h->write_pos + 1) % HISTORY_DEPTH;
    if (h->count < HISTORY_DEPTH) h->count++;
}

/* =========================================================================
 * Init
 * ========================================================================= */

void LineEditInit(LineEditState *st)
{
    memset(st, 0, sizeof(LineEditState));
    st->history.browse_pos = -1;
}

/* =========================================================================
 * LineEditRead — blocking readline with history tracking
 * ========================================================================= */

int LineEditRead(LineEditState *st, const char *prompt,
                 char *out, int max_len)
{
    if (!st || !out || max_len < 2) return LINE_ERROR;

    /* Print prompt */
    print(prompt);
    io_flush();

    /* Blocking readline through display daemon (proven working path) */
    int len = readline(out, (size_t)max_len);

    if (len < 0) return LINE_ERROR;
    if (len == 0) return LINE_EMPTY;

    /* Add to history for future recall */
    HistoryAdd(&st->history, out);

    return LINE_OK;
}
