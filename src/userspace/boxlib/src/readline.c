/*
 * readline.c — the console's line editor, one for every program.
 *
 * Keys are EVENTS. The display daemon lends the strand its ear
 * (DISP_CMD_LISTEN) for exactly as long as it reads, and republishes each key
 * as a Touch on the strand's own lane tag; without a daemon the strand hears
 * the "keyboard" tag itself. The editor never polls, never asks anybody for a
 * finished line and never waits for a reply it could misread: what it awaits
 * is a key, and the line is its own to keep. Between readings nobody holds
 * the ear, so a key typed early waits at the daemon for whoever reads next.
 *
 * Echo travels the road every print takes — the strand's lane — so it keeps
 * its place among everything else the strand said. Everything the editor does
 * to the caret is a STEP (console_step): a signed count of cells, resolved by
 * the console itself. Nothing here ever sends a '\b' — Canvas's backspace
 * erases the cell it steps onto but does nothing at column zero, which is
 * exactly where a line that wrapped past the right edge needs it to work.
 *
 * History is the cabin's, shared by its strands: recalled with Up and Down,
 * kept in order, and it grows with what was typed rather than stopping at a
 * number somebody guessed.
 */

#include "box/print.h"
#include "box/touch.h"
#include "box/keyboard.h"   /* kb_event_t, KB_MOD_*, KEY_* */
#include "box/memory.h"
#include "box/string.h"
#include "box/sync.h"

/* ===========================================================================
 * History — the cabin's, in order, unbounded
 * =========================================================================== */

typedef struct LineHistory {
    char   **lines;
    uint32_t count;
    uint32_t cap;
} LineHistory;

static LineHistory g_history;
static umutex_t    g_history_lock = UMUTEX_INIT;

/* Remember a line. The same line twice in a row is remembered once. */
static void HistoryKeep(const char *line, uint32_t len)
{
    if (len == 0) return;
    umutex_lock(&g_history_lock);
    if (g_history.count > 0 &&
        strcmp(g_history.lines[g_history.count - 1], line) == 0) {
        umutex_unlock(&g_history_lock);
        return;
    }
    if (g_history.count == g_history.cap) {
        uint32_t cap  = g_history.cap ? g_history.cap * 2 : 16;
        char   **more = (char **)realloc(g_history.lines, cap * sizeof(char *));
        if (!more) { umutex_unlock(&g_history_lock); return; }
        g_history.lines = more;
        g_history.cap   = cap;
    }
    char *copy = (char *)malloc(len + 1);
    if (copy) {
        memcpy(copy, line, len);
        copy[len] = '\0';
        g_history.lines[g_history.count++] = copy;
    }
    umutex_unlock(&g_history_lock);
}

/* Copy entry `index` (0 = oldest) into `out`; false when there is no such
 * entry. A copy, because another strand may grow the history meanwhile. */
static bool HistoryFetch(uint32_t index, char *out, uint32_t cap)
{
    bool have = false;
    umutex_lock(&g_history_lock);
    if (index < g_history.count) {
        const char *src = g_history.lines[index];
        uint32_t    n   = (uint32_t)strlen(src);
        if (n > cap - 1) n = cap - 1;
        memcpy(out, src, n);
        out[n] = '\0';
        have = true;
    }
    umutex_unlock(&g_history_lock);
    return have;
}

static uint32_t HistoryCount(void)
{
    umutex_lock(&g_history_lock);
    uint32_t n = g_history.count;
    umutex_unlock(&g_history_lock);
    return n;
}

/* ===========================================================================
 * The line under the cursor
 * =========================================================================== */

typedef struct EditLine {
    char    *buf;      /* the caller's buffer */
    uint32_t cap;      /* bytes of text it can hold (NUL excluded) */
    uint32_t len;
    uint32_t cursor;
    uint32_t browse;   /* history index while browsing; == count when not */
    char    *draft;    /* what was typed before browsing began, or NULL */
    uint32_t draft_len;
} EditLine;

/* Lay down `n` blanks in as few calls as the road takes.
 *
 * One call per blank is free on a lane — the run accumulates — and expensive
 * without a daemon, where every print_bytes is its own Manifest, its own
 * Canvas commit and its own blit. Recalling a two-character line over a
 * three-hundred-character one is 298 of them, which is the crawl this editor
 * was rewritten to remove. */
static void EchoBlanks(uint32_t n)
{
    /* Not a string: it is never read as one and a NUL would only cost a cell. */
    static const char spaces[32] = {
        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
    };
    while (n) {
        uint32_t chunk = n < sizeof(spaces) ? n : (uint32_t)sizeof(spaces);
        print_bytes(spaces, chunk);
        n -= chunk;
    }
}

static void EchoTail(const EditLine *l, uint32_t from, uint32_t trailing_blanks)
{
    /* Redraw buf[from..len), then `trailing_blanks` blanks that erase what a
     * shorter line leaves behind, then step back to the cursor. */
    if (l->len > from) print_bytes(l->buf + from, l->len - from);
    EchoBlanks(trailing_blanks);
    int32_t back = (int32_t)(l->len - l->cursor) + (int32_t)trailing_blanks;
    console_step(-back);
    io_flush();   /* an echo is seen as it is typed, not when a line is done */
}

static void Insert(EditLine *l, char c)
{
    if (l->len >= l->cap) return;               /* the caller's buffer is full */
    memmove(l->buf + l->cursor + 1, l->buf + l->cursor, l->len - l->cursor);
    l->buf[l->cursor] = c;
    l->len++;
    l->cursor++;
    EchoTail(l, l->cursor - 1, 0);
}

static void Backspace(EditLine *l)
{
    if (l->cursor == 0) return;
    memmove(l->buf + l->cursor - 1, l->buf + l->cursor, l->len - l->cursor);
    l->len--;
    l->cursor--;
    /* A STEP back, not a '\b'. Canvas '\b' erases the cell it steps onto and
     * does nothing at all at column zero, so on a line that had wrapped past
     * the right edge one character refused to disappear and every step after
     * it was one cell out. A step is linear across line ends; the erasing is
     * done by the blank EchoTail lays down after the tail. */
    console_step(-1);
    EchoTail(l, l->cursor, 1);
}

static void DeleteForward(EditLine *l)
{
    if (l->cursor == l->len) return;
    memmove(l->buf + l->cursor, l->buf + l->cursor + 1, l->len - l->cursor - 1);
    l->len--;
    EchoTail(l, l->cursor, 1);
}

static void MoveTo(EditLine *l, uint32_t where)
{
    if (where > l->len) where = l->len;
    console_step((int32_t)where - (int32_t)l->cursor);
    l->cursor = where;
}

/* Replace the whole line on screen and in the buffer.
 *
 * One step back to the first cell of the line, the new line laid over the old
 * one, and blanks for whatever the old one had beyond it. The old way walked
 * backwards one '\b' per character — a frame, a Manifest and a blit each, so
 * recalling a long line visibly crawled — and it could not erase a line that
 * had wrapped, because Canvas '\b' does nothing at column zero. */
static void Replace(EditLine *l, const char *text, uint32_t n)
{
    uint32_t was = l->len;
    if (n > l->cap) n = l->cap;

    console_step(-(int32_t)l->cursor);
    memcpy(l->buf, text, n);
    l->len    = n;
    l->cursor = n;
    if (n > 0) print_bytes(l->buf, n);

    uint32_t blanks = was > n ? was - n : 0;
    EchoBlanks(blanks);
    if (blanks) console_step(-(int32_t)blanks);
    io_flush();
}

static void Recall(EditLine *l, bool older)
{
    uint32_t count = HistoryCount();
    if (older) {
        if (l->browse == 0) return;
        if (l->browse == count) {
            /* Leaving the draft: keep it so Down can bring it back. */
            free(l->draft);
            l->draft = (char *)malloc(l->len + 1);
            if (l->draft) { memcpy(l->draft, l->buf, l->len); l->draft_len = l->len; }
        }
        l->browse--;
    } else {
        if (l->browse >= count) return;
        l->browse++;
        if (l->browse == count) {
            Replace(l, l->draft ? l->draft : "", l->draft ? l->draft_len : 0);
            return;
        }
    }
    char *entry = (char *)malloc(l->cap + 1);
    if (!entry) return;
    if (HistoryFetch(l->browse, entry, l->cap + 1))
        Replace(l, entry, (uint32_t)strlen(entry));
    free(entry);
}

/* ===========================================================================
 * Keys
 * =========================================================================== */

/* Wait for the next key on `ear`. False when the ear is gone (the strand has
 * no way to hear anything any more).
 *
 * ‼ WHAT COMES BACK IS CHECKED AGAINST THE EAR IT WAS ASKED FOR.
 *
 * touch_await takes a tag and parks the strand for it, but what it hands back
 * is whatever reached this strand's ring first — the tag governs the park, not
 * the answer (boxlib touch.c). A cabin that also wears "process:died" gets one
 * of those in the middle of a reading, and its twelve-byte payload is longer
 * than a key event, so it used to be copied into one and acted on: a character
 * nobody typed, or, with the right bit set, a phantom arrow that moves the
 * caret. Comparing the tag costs nothing and ends that whole class.
 *
 * The event of another tag is still consumed here, exactly as it was before —
 * that part is touch_await's to fix, and it needs a decision about how a bare
 * claim is supposed to hear its own key:value events. It is written down for
 * that conversation rather than guessed at here. */
static bool NextKey(TouchTag ear, kb_event_t *out)
{
    for (;;) {
        Touch t;
        if (touch_await(ear, &t, 0) != 0) return false;
        if (t.tag_id != ear) continue;
        if (t.payload_len < sizeof(kb_event_t)) continue;
        memcpy(out, t.payload, sizeof(kb_event_t));
        return true;
    }
}

int readline(char *buffer, size_t max_len)
{
    if (!buffer || max_len < 2) return -1;

    TouchTag ear = console_listen();
    if (ear == TOUCH_TAG_INVALID) return -1;

    EditLine l = {
        .buf = buffer, .cap = (uint32_t)(max_len - 1), .len = 0, .cursor = 0,
        .browse = HistoryCount(), .draft = NULL, .draft_len = 0,
    };

    for (;;) {
        kb_event_t k;
        if (!NextKey(ear, &k)) { free(l.draft); console_unlisten(); return -1; }

        if (k.mods & KB_MOD_EXTENDED) {
            switch (k.scancode) {
            case KEY_LEFT:   if (l.cursor > 0) MoveTo(&l, l.cursor - 1); break;
            case KEY_RIGHT:  MoveTo(&l, l.cursor + 1);                   break;
            case KEY_HOME:   MoveTo(&l, 0);                              break;
            case KEY_END:    MoveTo(&l, l.len);                          break;
            case KEY_UP:     Recall(&l, true);                           break;
            case KEY_DOWN:   Recall(&l, false);                          break;
            case KEY_DELETE: DeleteForward(&l);                          break;
            default: break;
            }
            continue;
        }

        char c = k.ascii;
        if (c == '\r' || c == '\n') {
            MoveTo(&l, l.len);
            print_bytes("\n", 1);
            buffer[l.len] = '\0';
            HistoryKeep(buffer, l.len);
            free(l.draft);
            io_flush();
            console_unlisten();
            return (int)l.len;
        }
        if (c == '\b' || c == 0x7F) { Backspace(&l); continue; }
        if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) Insert(&l, c);
    }
}

/* WEAK for the same reason as printf in print.c: boxcxx's getchar reads
 * through the same FILE as fgetc(stdin), so a byte pushed back with ungetc
 * comes back to it. This one cannot see that pushback, which is correct for
 * a C program that has no FILE and wrong for a C++ one that does. */
__attribute__((weak)) int getchar(void)
{
    TouchTag ear = console_listen();
    if (ear == TOUCH_TAG_INVALID) return -1;
    for (;;) {
        kb_event_t k;
        if (!NextKey(ear, &k)) { console_unlisten(); return -1; }
        if (!(k.mods & KB_MOD_EXTENDED) && k.ascii != 0) {
            console_unlisten();
            return (unsigned char)k.ascii;
        }
    }
}

int input(const char *prompt, char *buffer, size_t max_len)
{
    if (prompt) print(prompt);
    return readline(buffer, max_len);
}
