
#include "box/print.h"
#include "box/touch.h"
#include "box/keyboard.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/sync.h"


typedef struct LineHistory {
    char   **lines;
    uint32_t count;
    uint32_t cap;
} LineHistory;

static LineHistory g_history;
static umutex_t    g_history_lock = UMUTEX_INIT;

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


typedef struct EditLine {
    char    *buf;
    uint32_t cap;
    uint32_t len;
    uint32_t cursor;
    uint32_t browse;
    char    *draft;
    uint32_t draft_len;
} EditLine;

static void EchoBlanks(uint32_t n)
{
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
    if (l->len > from) print_bytes(l->buf + from, l->len - from);
    EchoBlanks(trailing_blanks);
    int32_t back = (int32_t)(l->len - l->cursor) + (int32_t)trailing_blanks;
    console_step(-back);
    io_flush();
}

static void Insert(EditLine *l, char c)
{
    if (l->len >= l->cap) return;
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