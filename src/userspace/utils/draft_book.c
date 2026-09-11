
#include "box/memory.h"
#include "box/string.h"
#include "box/convert.h"
#include "box/error.h"

#include "draft.h"

#define DRAFT_TAB_STOP    8
#define DRAFT_LINE_SEED   32
#define DRAFT_BOOK_SEED   64


void say_str(char *buf, size_t cap, const char *s)
{
    size_t pos = strlen(buf);
    while (*s && pos + 1 < cap) buf[pos++] = *s++;
    buf[pos] = '\0';
}

void say_uint(char *buf, size_t cap, uint32_t v)
{
    char tmp[16];
    uint_to_str(v, tmp, sizeof(tmp));
    say_str(buf, cap, tmp);
}

void say_err(char *buf, size_t cap, int rc)
{
    say_str(buf, cap, " (error ");
    say_uint(buf, cap, (uint32_t)box_errno_of(rc));
    say_str(buf, cap, ")");
}

void msg_begin(void) { s_msg[0] = '\0'; }
void msg_add(const char *s) { say_str(s_msg, sizeof(s_msg), s); }
void msg_num(uint32_t v) { say_uint(s_msg, sizeof(s_msg), v); }


static int line_reserve(DraftLine *l, uint32_t need)
{
    if (l->cap >= need) return 1;
    uint32_t cap = l->cap ? l->cap : DRAFT_LINE_SEED;
    while (cap < need) cap *= 2;
    char *grown = realloc(l->text, cap);
    if (!grown) return 0;
    l->text = grown;
    l->cap  = cap;
    return 1;
}

static int book_reserve(uint32_t need)
{
    if (g_book.cap >= need) return 1;
    uint32_t cap = g_book.cap ? g_book.cap : DRAFT_BOOK_SEED;
    while (cap < need) cap *= 2;
    DraftLine *grown = realloc(g_book.lines, cap * sizeof(DraftLine));
    if (!grown) return 0;
    g_book.lines = grown;
    g_book.cap   = cap;
    return 1;
}

static int book_open_line(uint32_t at)
{
    if (!book_reserve(g_book.count + 1)) return 0;
    memmove(&g_book.lines[at + 1], &g_book.lines[at],
            (g_book.count - at) * sizeof(DraftLine));
    g_book.lines[at].text = NULL;
    g_book.lines[at].len  = 0;
    g_book.lines[at].cap  = 0;
    g_book.count++;
    return 1;
}

static void book_drop_line(uint32_t at)
{
    free(g_book.lines[at].text);
    memmove(&g_book.lines[at], &g_book.lines[at + 1],
            (g_book.count - at - 1) * sizeof(DraftLine));
    g_book.count--;
}

int book_load(const char *bytes, uint32_t len)
{
    uint32_t start = 0;
    for (uint32_t i = 0; i <= len; i++) {
        if (i != len && bytes[i] != '\n') continue;

        uint32_t n = i - start;
        if (!book_reserve(g_book.count + 1)) return 0;
        DraftLine *l = &g_book.lines[g_book.count];
        l->text = NULL;
        l->len  = 0;
        l->cap  = 0;
        if (n) {
            if (!line_reserve(l, n)) return 0;
            memcpy(l->text, bytes + start, n);
            l->len = n;
        }
        g_book.count++;
        start = i + 1;
    }
    return 1;
}

uint32_t book_bytes(void)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < g_book.count; i++) total += g_book.lines[i].len;
    if (g_book.count) total += g_book.count - 1;
    return total;
}

char *book_join(uint32_t *out_len)
{
    uint32_t total = book_bytes();
    char *buf = malloc(total ? total : 1);
    if (!buf) return NULL;

    uint32_t pos = 0;
    for (uint32_t i = 0; i < g_book.count; i++) {
        if (i) buf[pos++] = '\n';
        memcpy(buf + pos, g_book.lines[i].text, g_book.lines[i].len);
        pos += g_book.lines[i].len;
    }
    *out_len = pos;
    return buf;
}


uint32_t col_after(uint32_t col, char c)
{
    if (c == '\t') return col + (DRAFT_TAB_STOP - col % DRAFT_TAB_STOP);
    return col + 1;
}

uint32_t width_of(const DraftLine *l, uint32_t nbytes)
{
    uint32_t w = 0;
    if (nbytes > l->len) nbytes = l->len;
    for (uint32_t i = 0; i < nbytes; i++) w = col_after(w, l->text[i]);
    return w;
}

static uint32_t byte_at_col(const DraftLine *l, uint32_t col)
{
    uint32_t w = 0;
    for (uint32_t i = 0; i < l->len; i++) {
        if (w >= col) return i;
        w = col_after(w, l->text[i]);
    }
    return l->len;
}

uint32_t text_rows(void) { return g_rows - 2; }


void mark_column(void)
{
    g_want = width_of(&g_book.lines[g_line], g_byte);
}

void no_memory(const char *what)
{
    msg_begin();
    msg_add("there is no memory for ");
    msg_add(what);
}

void insert_byte(char c)
{
    DraftLine *l = &g_book.lines[g_line];
    if (!line_reserve(l, l->len + 1)) { no_memory("another byte"); return; }
    memmove(l->text + g_byte + 1, l->text + g_byte, l->len - g_byte);
    l->text[g_byte] = c;
    l->len++;
    g_byte++;
    g_dirty = 1;
    mark_column();
}

void split_line(void)
{
    DraftLine tail = { NULL, 0, 0 };
    uint32_t  n    = g_book.lines[g_line].len - g_byte;

    if (n) {
        if (!line_reserve(&tail, n)) { no_memory("a new line"); return; }
        memcpy(tail.text, g_book.lines[g_line].text + g_byte, n);
        tail.len = n;
    }
    if (!book_open_line(g_line + 1)) {
        free(tail.text);
        no_memory("a new line");
        return;
    }
    g_book.lines[g_line + 1] = tail;
    g_book.lines[g_line].len = g_byte;

    g_line++;
    g_byte  = 0;
    g_want  = 0;
    g_dirty = 1;
}

static int join_lines(uint32_t at)
{
    DraftLine *head = &g_book.lines[at];
    DraftLine *tail = &g_book.lines[at + 1];

    if (tail->len) {
        if (!line_reserve(head, head->len + tail->len)) return 0;
        memcpy(head->text + head->len, tail->text, tail->len);
        head->len += tail->len;
    }
    book_drop_line(at + 1);
    return 1;
}

void backspace(void)
{
    if (g_byte > 0) {
        DraftLine *l = &g_book.lines[g_line];
        memmove(l->text + g_byte - 1, l->text + g_byte, l->len - g_byte);
        l->len--;
        g_byte--;
    } else if (g_line > 0) {
        uint32_t above = g_line - 1;
        uint32_t seam  = g_book.lines[above].len;
        if (!join_lines(above)) { no_memory("the joined line"); return; }
        g_line = above;
        g_byte = seam;
    } else {
        return;
    }
    g_dirty = 1;
    mark_column();
}

void delete_forward(void)
{
    DraftLine *l = &g_book.lines[g_line];
    if (g_byte < l->len) {
        memmove(l->text + g_byte, l->text + g_byte + 1, l->len - g_byte - 1);
        l->len--;
    } else if (g_line + 1 < g_book.count) {
        if (!join_lines(g_line)) { no_memory("the joined line"); return; }
    } else {
        return;
    }
    g_dirty = 1;
}

void move_left(void)
{
    if (g_byte > 0) {
        g_byte--;
    } else if (g_line > 0) {
        g_line--;
        g_byte = g_book.lines[g_line].len;
    }
    mark_column();
}

void move_right(void)
{
    if (g_byte < g_book.lines[g_line].len) {
        g_byte++;
    } else if (g_line + 1 < g_book.count) {
        g_line++;
        g_byte = 0;
    }
    mark_column();
}

void move_line(int down)
{
    if (down) {
        if (g_line + 1 >= g_book.count) return;
        g_line++;
    } else {
        if (g_line == 0) return;
        g_line--;
    }
    g_byte = byte_at_col(&g_book.lines[g_line], g_want);
}

void move_page(int down)
{
    uint32_t step = text_rows();
    if (down) {
        g_line += step;
        if (g_line >= g_book.count) g_line = g_book.count - 1;
    } else {
        g_line = (g_line > step) ? g_line - step : 0;
    }
    g_byte = byte_at_col(&g_book.lines[g_line], g_want);
}