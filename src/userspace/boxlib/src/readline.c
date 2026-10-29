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
 * A step is right for the glass and invisible to the log: the kernel mirrors
 * text into its log ring, never caret movement, so a saved log holds the keys
 * and not the line they became. An edited line therefore writes itself over
 * itself at Enter (RestateLine) — the same characters onto the same cells, so
 * the glass gains nothing and the ring gains the line that was handed over. A
 * line typed straight through says nothing: its echo already is the line.
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

/* Entry `index` (0 = oldest), whole, in a copy of its own the caller frees —
 * a copy, because another strand may grow the history meanwhile. NULL when
 * there is no such entry or no memory for it. */
static char *HistoryCopy(uint32_t index, uint32_t *out_len)
{
    char *copy = NULL;
    umutex_lock(&g_history_lock);
    if (index < g_history.count) {
        const char *src = g_history.lines[index];
        uint32_t    n   = (uint32_t)strlen(src);
        copy = (char *)malloc(n + 1);
        if (copy) {
            memcpy(copy, src, n + 1);
            *out_len = n;
        }
    }
    umutex_unlock(&g_history_lock);
    return copy;
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
    char    *buf;      /* the caller's buffer, or the line's own when it grows */
    uint32_t cap;      /* bytes of text it can hold (NUL excluded) */
    bool     grows;    /* the buffer is the line's own, and doubles as it fills */
    uint32_t len;
    uint32_t cursor;
    uint32_t browse;   /* history index while browsing; == count when not */
    char    *draft;    /* what was typed before browsing began, or NULL */
    uint32_t draft_len;
    bool     echo_diverged;  /* the echo stopped reading as the line; RestateLine */
} EditLine;

/* The first room a growing line is given; it doubles whenever the line fills
 * it, so a line is as long as the person types it. */
#define READLINE_FIRST_ROOM 64u

/* Room for `need` bytes of text: true when the buffer holds them — as it is,
 * or grown, for a line that grows. A caller's own buffer never grows. */
static bool EnsureRoom(EditLine *l, uint32_t need)
{
    if (need <= l->cap) return true;
    if (!l->grows) return false;
    uint32_t cap = l->cap;
    while (cap < need) {
        if (cap > UINT32_MAX / 2) return false;
        cap *= 2;
    }
    char *more = (char *)realloc(l->buf, (size_t)cap + 1);
    if (!more) return false;
    l->buf = more;
    l->cap = cap;
    return true;
}

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
        print_bytes_to_screen(spaces, chunk);
        n -= chunk;
    }
}

static void EchoTail(EditLine *l, uint32_t from, uint32_t trailing_blanks)
{
    /* Two things here write over ground this line already covered: a blank
     * that erases a character, and a tail said a second time because the
     * caret sits inside the line. Both leave the log holding characters that
     * are not the line any more — RestateLine explains what is then owed.
     * Appending at the end covers nothing, which is why a line typed straight
     * through never sets this and never restates itself. */
    if (trailing_blanks > 0 || l->cursor < l->len) l->echo_diverged = true;

    /* Redraw buf[from..len), then `trailing_blanks` blanks that erase what a
     * shorter line leaves behind, then step back to the cursor. */
    if (l->len > from) print_bytes_to_screen(l->buf + from, l->len - from);
    EchoBlanks(trailing_blanks);
    int32_t back = (int32_t)(l->len - l->cursor) + (int32_t)trailing_blanks;
    console_step(-back);
    io_flush();   /* an echo is seen as it is typed, not when a line is done */
}

static void Insert(EditLine *l, char c)
{
    if (!EnsureRoom(l, l->len + 1)) return;     /* the caller's buffer is full */
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
    if (!EnsureRoom(l, n)) n = l->cap;

    /* A recall laid over a line that had characters says them both, in the
     * one place the log cannot tell them apart — see EchoTail's note and
     * RestateLine. Laid over nothing it says only the new line, and the log
     * is right about it without being told again. */
    if (was > 0) l->echo_diverged = true;

    console_step(-(int32_t)l->cursor);
    memcpy(l->buf, text, n);
    l->len    = n;
    l->cursor = n;
    if (n > 0) print_bytes_to_screen(l->buf, n);

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
    uint32_t n     = 0;
    char    *entry = HistoryCopy(l->browse, &n);
    if (!entry) return;
    Replace(l, entry, n);
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
 * That guard has since been made unnecessary at its source: touch_await now
 * answers only with the tag it was asked for, and an event of another tag goes
 * to the per-strand stash for its own consumer instead of being consumed here
 * and thrown away. The compare is kept all the same — it costs one integer and
 * it is the line that states, where a reader will look, that these bytes are a
 * key because the tag says so and not because the length allows it. */
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

/* Say the accepted line into the kernel's log ring — by writing it over
 * itself, so the glass gains nothing at all.
 *
 * ‼ THE LOG KEEPS WHAT THE CONSOLE WAS TOLD, AND A CARET STEP TELLS IT
 * NOTHING. The hardware deck mirrors into the log ring every character that
 * reaches the screen (hardware_ops.c, HwVgaPutString) and no cursor movement
 * whatsoever (HwVgaStepCursor). So the ring holds every key that was echoed
 * and no record that any of them was taken back: on the board run of
 * 2026-09-20 watch.log carried "memtagt" directly above the output of a
 * `memtag` whose last letter had been erased before Enter — a log naming a
 * command this machine never ran.
 *
 * The line is therefore said once more at Enter, and said the only way a
 * program can reach that ring without spending a line of the person's screen
 * on it: the caret steps back over the line and the accepted buffer is
 * printed onto the cells that already hold it. The same characters land on
 * the same cells, so the glass does not change and nothing is duplicated in
 * front of anybody; the ring gains the accepted line immediately before the
 * newline, which makes it the last word on what was handed over. Speaking it
 * instead — kdbg — costs a printed paragraph per edited line, because the one
 * operation that writes the ring from userspace, HW_DEBUG_PRINT, goes through
 * kprintf and kprintf writes the glass too.
 *
 * The backward step is the one Home and a history recall already take across
 * the same line, so it is sound exactly where those are.
 *
 * A line edited down to NOTHING has nothing to write and stays silent: the
 * ring keeps the withdrawn keys with no line after them. Correcting that
 * needs a sentence, and a sentence is a line on the glass nobody typed. It
 * wants a ring-only operation on the hardware deck, which this cabin has no
 * way to ask for today. */
static void RestateLine(const EditLine *l)
{
    if (l->len == 0) return;
    console_step(-(int32_t)l->len);
    print_bytes_to_screen(l->buf, l->len);
}

/* Keys edited into `l` until Enter: the line's length, NUL-terminated in
 * l->buf, or -1 when the ear is gone. */
static int EditUntilEnter(EditLine *l)
{
    TouchTag ear = console_listen();
    if (ear == TOUCH_TAG_INVALID) return -1;

    for (;;) {
        kb_event_t k;
        if (!NextKey(ear, &k)) { free(l->draft); console_unlisten(); return -1; }

        if (k.mods & KB_MOD_EXTENDED) {
            switch (k.scancode) {
            case KEY_LEFT:   if (l->cursor > 0) MoveTo(l, l->cursor - 1); break;
            case KEY_RIGHT:  MoveTo(l, l->cursor + 1);                    break;
            case KEY_HOME:   MoveTo(l, 0);                                break;
            case KEY_END:    MoveTo(l, l->len);                           break;
            case KEY_UP:     Recall(l, true);                             break;
            case KEY_DOWN:   Recall(l, false);                            break;
            case KEY_DELETE: DeleteForward(l);                            break;
            default: break;
            }
            continue;
        }

        char c = k.ascii;
        if (c == '\r' || c == '\n') {
            MoveTo(l, l->len);
            /* Before the newline, so the line the log ends up holding is the
             * one that was accepted and not the keys it was edited out of. */
            if (l->echo_diverged) RestateLine(l);
            print_bytes_to_screen("\n", 1);
            l->buf[l->len] = '\0';
            HistoryKeep(l->buf, l->len);
            free(l->draft);
            io_flush();
            console_unlisten();
            return (int)l->len;
        }
        if (c == '\b' || c == 0x7F) { Backspace(l); continue; }
        if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) Insert(l, c);
    }
}

int readline(char *buffer, size_t max_len)
{
    if (!buffer || max_len < 2) return -1;
    EditLine l = {
        .buf = buffer, .cap = (uint32_t)(max_len - 1), .grows = false, .len = 0,
        .cursor = 0, .browse = HistoryCount(), .draft = NULL, .draft_len = 0,
        .echo_diverged = false,
    };
    return EditUntilEnter(&l);
}

int readline_grow(char **line)
{
    if (!line) return -1;
    *line = NULL;
    EditLine l = {
        .buf = (char *)malloc(READLINE_FIRST_ROOM + 1), .cap = READLINE_FIRST_ROOM,
        .grows = true, .len = 0, .cursor = 0, .browse = HistoryCount(), .draft = NULL,
        .draft_len = 0, .echo_diverged = false,
    };
    if (!l.buf) return -1;
    int len = EditUntilEnter(&l);
    if (len < 0) {
        free(l.buf);
        return -1;
    }
    *line = l.buf;
    return len;
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
    if (prompt) print_to_screen(prompt);
    return readline(buffer, max_len);
}
