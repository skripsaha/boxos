
#include "box/vga.h"
#include "box/color.h"
#include "box/string.h"

#include "draft.h"

#define DRAFT_TITLE_FG    COLOR_WHITE
#define DRAFT_TITLE_BG    COLOR_SLATE
#define DRAFT_TEXT_FG     COLOR_LIGHT_GRAY
#define DRAFT_TEXT_BG     COLOR_BLACK
#define DRAFT_ODD_FG      COLOR_DARK_GRAY
#define DRAFT_HINT_FG     COLOR_DARK_GRAY
#define DRAFT_MSG_FG      COLOR_AMBER
#define DRAFT_CMD_FG      COLOR_WHITE

static uint32_t g_top;
static uint32_t g_left;

static char s_right[64];


static void frame_cell(uint32_t row, uint32_t col, char ch, Color fg, Color bg)
{
    if (row >= g_rows || col >= g_cols) return;
    TextCell *c = &g_frame[row * g_cols + col];
    c->fg     = fg;
    c->bg     = bg;
    c->ch     = ch;
    c->pad[0] = 0;
    c->pad[1] = 0;
    c->pad[2] = 0;
}

static uint32_t frame_say(uint32_t row, uint32_t col, const char *s,
                          Color fg, Color bg)
{
    while (*s && col < g_cols) {
        unsigned char u = (unsigned char)*s++;
        frame_cell(row, col++, (u >= 0x20 && u < 0x7F) ? (char)u : '?', fg, bg);
    }
    return col;
}

static void frame_band(uint32_t row, Color fg, Color bg)
{
    for (uint32_t c = 0; c < g_cols; c++) frame_cell(row, c, ' ', fg, bg);
}

static void paint_title(void)
{
    frame_band(0, DRAFT_TITLE_FG, DRAFT_TITLE_BG);

    uint32_t col = frame_say(0, 1, "draft", DRAFT_TITLE_FG, DRAFT_TITLE_BG);
    col = frame_say(0, col + 2, s_name, DRAFT_TITLE_FG, DRAFT_TITLE_BG);
    if (s_tagline[0])
        col = frame_say(0, col + 2, s_tagline, DRAFT_TITLE_FG, DRAFT_TITLE_BG);

    s_right[0] = '\0';
    say_str(s_right, sizeof(s_right), "line ");
    say_uint(s_right, sizeof(s_right), g_line + 1);
    say_str(s_right, sizeof(s_right), "/");
    say_uint(s_right, sizeof(s_right), g_book.count);
    say_str(s_right, sizeof(s_right), "  col ");
    say_uint(s_right, sizeof(s_right), width_of(&g_book.lines[g_line], g_byte) + 1);
    if (g_dirty) say_str(s_right, sizeof(s_right), "  *");

    uint32_t rlen = (uint32_t)strlen(s_right);
    if (rlen + 1 < g_cols && col + 2 <= g_cols - rlen - 1)
        frame_say(0, g_cols - rlen - 1, s_right, DRAFT_TITLE_FG, DRAFT_TITLE_BG);
}

static void paint_line(uint32_t row, const DraftLine *l)
{
    uint32_t right = g_left + g_cols;
    uint32_t w     = 0;

    for (uint32_t i = 0; i < l->len && w < right; i++) {
        char          c = l->text[i];
        unsigned char u = (unsigned char)c;

        if (c == '\t') {
            uint32_t stop = col_after(w, '\t');
            for (; w < stop && w < right; w++)
                if (w >= g_left)
                    frame_cell(row, w - g_left, ' ', DRAFT_TEXT_FG, DRAFT_TEXT_BG);
        } else if (u >= 0x20 && u < 0x7F) {
            if (w >= g_left)
                frame_cell(row, w - g_left, c, DRAFT_TEXT_FG, DRAFT_TEXT_BG);
            w++;
        } else {
            if (w >= g_left)
                frame_cell(row, w - g_left, '?', DRAFT_ODD_FG, DRAFT_TEXT_BG);
            w++;
        }
    }
    if (g_left > 0 && l->len > 0)
        frame_cell(row, 0, '<', DRAFT_ODD_FG, DRAFT_TEXT_BG);
}

static void paint_text(void)
{
    uint32_t rows = text_rows();
    for (uint32_t r = 0; r < rows; r++) {
        frame_band(r + 1, DRAFT_TEXT_FG, DRAFT_TEXT_BG);
        uint32_t li = g_top + r;
        if (li < g_book.count) paint_line(r + 1, &g_book.lines[li]);
    }
}

static void paint_foot(void)
{
    uint32_t row = g_rows - 1;
    frame_band(row, DRAFT_TEXT_FG, DRAFT_TEXT_BG);

    if (g_cmd_open) {
        frame_cell(row, 0, ':', DRAFT_CMD_FG, DRAFT_TEXT_BG);
        frame_say(row, 2, s_cmd, DRAFT_CMD_FG, DRAFT_TEXT_BG);
    } else if (s_msg[0]) {
        frame_say(row, 1, s_msg, DRAFT_MSG_FG, DRAFT_TEXT_BG);
    } else {
        frame_say(row, 1, "^S save   ^Q quit   ESC command",
                  DRAFT_HINT_FG, DRAFT_TEXT_BG);
    }
}

static void follow_cursor(void)
{
    uint32_t rows = text_rows();
    if (g_line < g_top)              g_top = g_line;
    else if (g_line >= g_top + rows) g_top = g_line - rows + 1;

    uint32_t col = width_of(&g_book.lines[g_line], g_byte);
    if (col < g_left)                g_left = col;
    else if (col >= g_left + g_cols) g_left = col - g_cols + 1;
}

void paint(void)
{
    follow_cursor();
    paint_title();
    paint_text();
    paint_foot();

    uint32_t crow, ccol;
    if (g_cmd_open) {
        crow = g_rows - 1;
        ccol = 2 + g_cmd_len;
        if (ccol >= g_cols) ccol = g_cols - 1;
    } else {
        crow = 1 + g_line - g_top;
        ccol = width_of(&g_book.lines[g_line], g_byte) - g_left;
    }
    (void)vga_setcursor((uint8_t)crow, (uint8_t)ccol);

    (void)vga_paint(0, 0, (uint8_t)g_rows, (uint8_t)g_cols, g_frame);
}