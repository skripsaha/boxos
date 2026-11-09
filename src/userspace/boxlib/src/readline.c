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
 * the console itself. Nothing here sends a '\b' to a console with a caret —
 * its backspace erases the cell it steps onto but does nothing at column
 * zero, which is exactly where a line that wrapped past the right edge needs
 * it to work.
 *
 * A step is right for the glass and invisible to the log: the display says
 * every run of text it draws into the log ring, and a caret step is no text,
 * so a saved log holds the keys and not the line they became. An edited line
 * therefore writes itself over itself at Enter (RestateLine) — the same
 * characters onto the same cells, so the glass gains nothing and the ring
 * gains the line that was handed over. A line typed straight through says
 * nothing: its echo already is the line.
 *
 * Without a display there is no caret at all (console_step says so), and the
 * echo goes the way a teletype's does: into the log ring, which the kernel's
 * Emergency Screen draws and the serial line carries, and where '\b' takes
 * back the cell it steps onto. A teletype edits a line only at its end: keys
 * typed onto it, the last one rubbed out with "\b \b", a recall rubbing the
 * whole line out and typing the other; the caret keys move it no further back
 * than it is, and nothing is laid dim. It is asked at every edit, so a display
 * that dies in the middle of a line leaves the rest of the line to the
 * teletype.
 *
 * History is the cabin's, shared by its strands: recalled with Up and Down,
 * kept in order, and it grows with what was typed rather than stopping at a
 * number somebody guessed. A line read with a Whisper walks the whisperer's
 * lines instead, and is kept by the whisperer, not here.
 *
 * A Whisper is what the line could go on to be, laid dim past the caret after
 * every key that leaves the caret at the end of the line, and taken back with
 * blanks as the line changes. Both are hints (console_hint): on the glass in
 * their place among the echo, and never in the log ring, which keeps what was
 * typed and not what was suggested. The dim cells are always past the end of
 * the line; `dim_end` remembers how far they reach, so the next whisper covers
 * what the last one left.
 */

#include "box/display.h"
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

/* The entry `back` lines before the newest (1 = the newest), whole, in a copy
 * of its own the caller frees — a copy, because another strand may grow the
 * history meanwhile. NULL when there is no such entry or no memory for it. */
static char *HistoryCopy(uint32_t back, uint32_t *out_len)
{
    char *copy = NULL;
    umutex_lock(&g_history_lock);
    if (back >= 1 && back <= g_history.count) {
        const char *src = g_history.lines[g_history.count - back];
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

/* ===========================================================================
 * The line under the cursor
 * =========================================================================== */

typedef struct EditLine {
    char    *buf;      /* the caller's buffer, or the line's own when it grows */
    uint32_t cap;      /* bytes of text it can hold (NUL excluded) */
    bool     grows;    /* the buffer is the line's own, and doubles as it fills */
    uint32_t len;
    uint32_t cursor;
    uint32_t back;     /* lines back from the one being typed while recalling; 0 when not */
    char    *draft;    /* what was typed before recalling began, or NULL */
    uint32_t draft_len;
    bool     echo_diverged;  /* the echo stopped reading as the line; RestateLine */
    LineWhisper *whisper;    /* NULL: the cabin's history and nothing whispered */
    char    *said;     /* what the whisper says the line could go on with, or NULL */
    uint32_t said_len;
    uint32_t dim_end;  /* the offset in the line the dim cells reach; 0 when none */
} EditLine;

/* Dim enough to read as not typed yet, on a framebuffer and on the sixteen
 * colours of a text console alike. */
#define READLINE_WHISPER_COLOR COLOR_DARK_GRAY

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

/* Lay down `n` blanks in as few calls as the road takes — text, or hints when
 * they take a hint back.
 *
 * One call per blank is free on a lane — the run accumulates — and expensive
 * without a daemon, where every print_bytes is its own call into the kernel.
 * Recalling a two-character line over a three-hundred-character one is 298 of
 * them, which is the crawl this editor was rewritten to remove. */
static void EchoBlanks(uint32_t n, bool hint)
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
        if (hint) console_hint(spaces, chunk);
        else      print_bytes_to_screen(spaces, chunk);
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
    EchoBlanks(trailing_blanks, false);
    int32_t back = (int32_t)(l->len - l->cursor) + (int32_t)trailing_blanks;
    console_step(-back);
    io_flush();   /* an echo is seen as it is typed, not when a line is done */
}

static void MoveTo(EditLine *l, uint32_t where);

static void Insert(EditLine *l, const char *text, uint32_t n)
{
    if (!console_step(0)) MoveTo(l, l->len);          /* a teletype types at the end */
    if (n == 0 || !EnsureRoom(l, l->len + n)) return;   /* the caller's buffer is full */
    memmove(l->buf + l->cursor + n, l->buf + l->cursor, l->len - l->cursor);
    memcpy(l->buf + l->cursor, text, n);
    l->len    += n;
    l->cursor += n;
    EchoTail(l, l->cursor - n, 0);
}

/* What the editor would have typed itself: a character is one cell, so the
 * steps it counts are the cells it laid. */
static bool Typeable(const char *text, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] >= 0x7F) return false;
    }
    return true;
}

/* `n` cells of `text` laid dim past the end of the line, blanks over whatever
 * the dim cells laid before reach beyond them, and the caret stepped back to
 * where it was — at the end of the line, the only place anything dim is laid. */
static void DimDraw(EditLine *l, const char *text, uint32_t n)
{
    if (!console_step(0)) {   /* a teletype lays nothing it would have to take back */
        l->dim_end = 0;
        return;
    }
    uint32_t reach  = l->len + n;
    uint32_t blanks = l->dim_end > reach ? l->dim_end - reach : 0;
    l->dim_end = n ? reach : 0;
    if (n + blanks == 0) return;

    if (n) {
        Color was = get_color();
        set_color(READLINE_WHISPER_COLOR);
        console_hint(text, n);
        set_color(was);
    }
    EchoBlanks(blanks, true);
    console_step(-(int32_t)(n + blanks));
    io_flush();
}

/* The dim cells taken away, before the caret leaves the end of the line. */
static void DimHide(EditLine *l)
{
    if (l->dim_end) DimDraw(l, NULL, 0);
}

/* What the whisper says now: laid dim when the caret is at the end of a line
 * that has something on it, nothing otherwise. The same words laid again are
 * not laid twice. */
static void WhisperRefresh(EditLine *l)
{
    if (!l->whisper) return;

    char    *said = NULL;
    uint32_t n    = 0;
    if (l->cursor == l->len && l->len > 0) {
        l->buf[l->len] = '\0';
        said = l->whisper->Continue(l->whisper, l->buf, l->len);
        n    = said ? (uint32_t)strlen(said) : 0;
        if (n == 0 || !Typeable(said, n)) {
            free(said);
            said = NULL;
            n    = 0;
        }
    }

    bool same = said && l->said && n == l->said_len && memcmp(said, l->said, n) == 0 &&
                l->dim_end == l->len + n;
    free(l->said);
    l->said     = said;
    l->said_len = n;
    if (!same && l->cursor == l->len) DimDraw(l, said, n);
}

/* How much of `text` is its next word: the blanks before it, then up to the
 * next blank outside "double quotes". */
static uint32_t WordReach(const char *text, uint32_t n)
{
    uint32_t at     = 0;
    bool     quoted = false;
    while (at < n && text[at] == ' ') at++;
    while (at < n && (quoted || text[at] != ' ')) {
        if (text[at] == '"') quoted = !quoted;
        at++;
    }
    return at;
}

/* Tab: the next word of the whisper, as if typed — all of it with Shift. With
 * nothing whispered, what the line still needs, laid dim and never typed.
 * True when that was laid: it stays until the next key. */
static bool WhisperTab(EditLine *l, bool all)
{
    if (!l->whisper || l->cursor != l->len) return false;
    if (l->said) {
        Insert(l, l->said, all ? l->said_len : WordReach(l->said, l->said_len));
        return false;
    }

    l->buf[l->len] = '\0';
    char    *needs = l->whisper->Needs(l->whisper, l->buf, l->len);
    uint32_t n     = needs ? (uint32_t)strlen(needs) : 0;
    if (n > 0 && Typeable(needs, n)) DimDraw(l, needs, n);
    free(needs);
    return true;
}

static void Backspace(EditLine *l)
{
    if (!console_step(0)) {
        MoveTo(l, l->len);
        if (l->len == 0) return;
        l->len--;
        l->cursor--;
        print_bytes_to_screen("\b \b", 3);   /* back, blank, back: a rub-out */
        io_flush();
        return;
    }
    if (l->cursor == 0) return;
    memmove(l->buf + l->cursor - 1, l->buf + l->cursor, l->len - l->cursor);
    l->len--;
    l->cursor--;
    /* A STEP back, not a '\b'. The console's '\b' erases the cell it steps
     * onto and does nothing at all at column zero, so on a line that had
     * wrapped past the right edge one character refused to disappear and
     * every step after it was one cell out. A step is linear across line
     * ends; the erasing is done by the blank EchoTail lays down after the
     * tail. */
    console_step(-1);
    EchoTail(l, l->cursor, 1);
}

static void DeleteForward(EditLine *l)
{
    if (l->cursor == l->len || !console_step(0)) return;
    memmove(l->buf + l->cursor, l->buf + l->cursor + 1, l->len - l->cursor - 1);
    l->len--;
    EchoTail(l, l->cursor, 1);
}

static void MoveTo(EditLine *l, uint32_t where)
{
    if (where > l->len) where = l->len;
    if (!console_step(0)) {
        /* A teletype goes on only by typing what it passes over, and back
         * not at all. */
        if (where > l->cursor) {
            print_bytes_to_screen(l->buf + l->cursor, where - l->cursor);
            io_flush();
            l->cursor = where;
        }
        return;
    }
    if (where < l->len) DimHide(l);
    console_step((int32_t)where - (int32_t)l->cursor);
    l->cursor = where;
}

/* Replace the whole line on screen and in the buffer.
 *
 * One step back to the first cell of the line, the new line laid over the old
 * one, and blanks for whatever the old one had beyond it. The old way walked
 * backwards one '\b' per character — a frame and a call into the kernel each,
 * so recalling a long line visibly crawled — and it could not erase a line
 * that had wrapped, because '\b' does nothing at column zero. A teletype has
 * only that way: the old line rubbed out from its end, the new one typed. */
static void Replace(EditLine *l, const char *text, uint32_t n)
{
    uint32_t was = l->len;
    if (!EnsureRoom(l, n)) n = l->cap;

    if (!console_step(0)) {
        /* "\b \b" for each character, eight to a call. A rub-out stops at the
         * left edge of the glass, so a line that wrapped keeps its first rows
         * standing there. */
        static const char rub[] = "\b \b\b \b\b \b\b \b\b \b\b \b\b \b\b \b";
        MoveTo(l, l->len);
        for (uint32_t left = was; left > 0;) {
            uint32_t chunk = left < 8 ? left : 8;
            print_bytes_to_screen(rub, chunk * 3);
            left -= chunk;
        }
        memcpy(l->buf, text, n);
        l->len    = n;
        l->cursor = n;
        if (n > 0) print_bytes_to_screen(l->buf, n);
        io_flush();
        return;
    }

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
    EchoBlanks(blanks, false);
    if (blanks) console_step(-(int32_t)blanks);
    io_flush();
}

/* The line `back` lines before the one being typed (1 = the newest): the
 * whisperer's, or the cabin's. A copy the caller frees; NULL past the oldest,
 * and for a line the editor could not have typed. */
static char *RecallCopy(const EditLine *l, uint32_t back, uint32_t *out_len)
{
    if (!l->whisper) return HistoryCopy(back, out_len);

    char *line = l->whisper->Recall(l->whisper, back - 1);
    if (!line) return NULL;
    uint32_t n = (uint32_t)strlen(line);
    if (!Typeable(line, n)) {
        free(line);
        return NULL;
    }
    *out_len = n;
    return line;
}

static void Recall(EditLine *l, bool older)
{
    if (!older && l->back == 0) return;
    uint32_t back = older ? l->back + 1 : l->back - 1;
    if (back == 0) {
        l->back = 0;
        Replace(l, l->draft ? l->draft : "", l->draft ? l->draft_len : 0);
        return;
    }

    uint32_t n     = 0;
    char    *entry = RecallCopy(l, back, &n);
    if (!entry) return;
    if (l->back == 0) {
        /* Leaving the draft: keep it so Down can bring it back. */
        free(l->draft);
        l->draft = (char *)malloc(l->len + 1);
        if (l->draft) { memcpy(l->draft, l->buf, l->len); l->draft_len = l->len; }
    }
    l->back = back;
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
typedef enum { HEARD_KEY, HEARD_END, HEARD_NOTHING } Heard;

/* A key — three bytes, kb_event_t — or the display's word that the program is
 * asked to finish (box/display.h ConsoleNotice), told apart by length; anything
 * else said on the tag is passed over. */
static Heard NextKey(TouchTag ear, kb_event_t *out)
{
    for (;;) {
        Touch t;
        if (touch_await(ear, &t, 0) != 0) return HEARD_NOTHING;
        if (t.tag_id != ear) continue;
        if (console_notice_of(t.payload, t.payload_len) == CONSOLE_NOTICE_END) return HEARD_END;
        if (t.payload_len != sizeof(kb_event_t)) continue;
        memcpy(out, t.payload, sizeof(kb_event_t));
        return HEARD_KEY;
    }
}

/* Say the accepted line into the kernel's log ring — by writing it over
 * itself, so the glass gains nothing at all.
 *
 * ‼ THE LOG KEEPS WHAT THE CONSOLE WAS TOLD, AND A CARET STEP TELLS IT
 * NOTHING. The display says into the log ring every run of text it draws
 * (display.cpp) and no caret movement whatsoever, because a step is no text.
 * So the ring holds every key that was echoed and no record that any of them
 * was taken back: on the board run of 2026-09-20 watch.log carried "memtagt"
 * directly above the output of a `memtag` whose last letter had been erased
 * before Enter — a log naming a command this machine never ran.
 *
 * The line is therefore said once more at Enter, and said the only way a
 * program can reach that ring without spending a line of the person's screen
 * on it: the caret steps back over the line and the accepted buffer is
 * printed onto the cells that already hold it. The same characters land on
 * the same cells, so the glass does not change and nothing is duplicated in
 * front of anybody; the ring gains the accepted line immediately before the
 * newline, which makes it the last word on what was handed over. Speaking it
 * instead — kdbg — costs a printed paragraph per edited line: whatever is
 * said into the ring is drawn on the glass too, because the glass and the
 * ring are one account.
 *
 * The backward step is the one Home and a history recall already take across
 * the same line, so it is sound exactly where those are. A teletype has no
 * such step, and needs none: what it echoed — "\b \b" and all — is in the
 * ring as typed, and any terminal reading it shows the line that was handed
 * over.
 *
 * A line edited down to NOTHING has nothing to write and stays silent: the
 * ring keeps the withdrawn keys with no line after them. Correcting that
 * needs a sentence, and a sentence is a line on the glass nobody typed. */
static void RestateLine(const EditLine *l)
{
    if (l->len == 0 || !console_step(0)) return;
    console_step(-(int32_t)l->len);
    print_bytes_to_screen(l->buf, l->len);
}

/* One key that is not Enter, edited into the line, and what the whisper says
 * after it. */
static void EditKey(EditLine *l, const kb_event_t *k)
{
    if (k->mods & KB_MOD_EXTENDED) {
        switch (k->scancode) {
        case KEY_LEFT:   if (l->cursor > 0) MoveTo(l, l->cursor - 1); break;
        case KEY_RIGHT:  MoveTo(l, l->cursor + 1);                    break;
        case KEY_HOME:   MoveTo(l, 0);                                break;
        case KEY_END:    MoveTo(l, l->len);                           break;
        case KEY_UP:     Recall(l, true);                             break;
        case KEY_DOWN:   Recall(l, false);                            break;
        case KEY_DELETE: DeleteForward(l);                            break;
        default: break;
        }
        WhisperRefresh(l);
        return;
    }

    char c = k->ascii;
    if (c == '\t') {
        if (WhisperTab(l, (k->mods & KB_MOD_SHIFT) != 0)) return;
    } else if (c == '\b' || c == 0x7F) {
        Backspace(l);
    } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) {
        Insert(l, &c, 1);
    }
    WhisperRefresh(l);
}

/* The line handed over: the dim cells taken away, the line said whole, the
 * newline. Kept in the cabin's history unless a whisperer keeps its own. */
static int Accept(EditLine *l)
{
    MoveTo(l, l->len);
    DimHide(l);
    /* Before the newline, so the line the log ends up holding is the one that
     * was accepted and not the keys it was edited out of. */
    if (l->echo_diverged) RestateLine(l);
    print_bytes_to_screen("\n", 1);
    l->buf[l->len] = '\0';
    if (!l->whisper) HistoryKeep(l->buf, l->len);
    io_flush();
    return (int)l->len;
}

/* Keys edited into `l` until Enter: the line's length, NUL-terminated in
 * l->buf; -1 when the ear is gone, READLINE_ASKED_TO_END when the program is
 * asked to finish — the line left as it stands on the screen, a line end after
 * it. */
static int EditUntilEnter(EditLine *l)
{
    TouchTag ear = console_listen();
    if (ear == TOUCH_TAG_INVALID) return -1;

    int len = -1;
    for (;;) {
        kb_event_t k;
        Heard      heard = NextKey(ear, &k);
        if (heard == HEARD_END) {
            print_bytes_to_screen("\n", 1);
            io_flush();
            len = READLINE_ASKED_TO_END;
        }
        if (heard != HEARD_KEY) break;
        if (k.ascii == '\r' || k.ascii == '\n') {
            len = Accept(l);
            break;
        }
        EditKey(l, &k);
    }
    free(l->draft);
    free(l->said);
    console_unlisten();
    return len;
}

int readline(char *buffer, size_t max_len)
{
    if (!buffer || max_len < 2) return -1;
    EditLine l = {
        .buf = buffer, .cap = (uint32_t)(max_len - 1), .grows = false,
    };
    return EditUntilEnter(&l);
}

/* A line of its own that grows as it is typed, edited with `whisper` (NULL
 * for none): its length and *line, or -1 or READLINE_ASKED_TO_END with *line
 * NULL. */
static int ReadGrowing(char **line, LineWhisper *whisper)
{
    if (!line) return -1;
    *line = NULL;
    EditLine l = {
        .buf = (char *)malloc(READLINE_FIRST_ROOM + 1), .cap = READLINE_FIRST_ROOM,
        .grows = true, .whisper = whisper,
    };
    if (!l.buf) return -1;
    int len = EditUntilEnter(&l);
    if (len < 0) {
        free(l.buf);
        return len;
    }
    *line = l.buf;
    return len;
}

int readline_grow(char **line)
{
    return ReadGrowing(line, NULL);
}

int readline_whispered(char **line, LineWhisper *whisper)
{
    return ReadGrowing(line, whisper);
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
        if (NextKey(ear, &k) != HEARD_KEY) { console_unlisten(); return -1; }
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
