#include "box/print.h"
#include "box/vga.h"
#include "box/color.h"
#include "box/keyboard.h"
#include "box/touch.h"
#include "box/clock.h"
#include "box/cpu.h"
#include "box/luggage.h"
#include "box/system.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/convert.h"
#include "box/error.h"

#define ASPECT              2.0
#define TWO_PI              6.28318530717958647692

#define RIM_LEVEL           0.55
#define EDGE_LEVEL          1.00
#define CORE_LEVEL          2.50

#define RADIUS_SMALL_DIV    18.0
#define RADIUS_LARGE_DIV     9.0

#define RISE_PERIOD_MIN_S   24.0
#define RISE_PERIOD_MAX_S   40.0
#define SIDEWAYS_SECONDS    35.0
#define NUDGE_FRACTION       0.10
#define ADVANCE_MAX_S        0.25

#define CELLS_PER_BLOB      400
#define BLOB_CEILING_MIN      4
#define BLOB_DEFAULT          6
#define FLOW_MIN              1
#define FLOW_MAX              9
#define FLOW_DEFAULT          5

#define FRAME_US            40000u

#define DEFAULT_LIQUID      COLOR_RGB(0xFF, 0x3A, 0x6B)
#define DEFAULT_EDGE        COLOR_RGB(0x7A, 0x10, 0x40)
#define DEFAULT_BACK        COLOR_RGB(0x10, 0x08, 0x20)

#define MENU_FG             COLOR_LIGHT_GRAY
#define MENU_BG             COLOR_RGB(0x10, 0x12, 0x18)
#define MENU_SEL_FG         COLOR_WHITE
#define MENU_SEL_BG         COLOR_RGB(0x28, 0x30, 0x40)
#define MENU_TITLE_FG       COLOR_AMBER
#define MENU_EDIT_FG        COLOR_WHITE
#define MENU_EDIT_BG        COLOR_RGB(0x48, 0x2A, 0x10)
#define MENU_NOTE_FG        COLOR_RED

#define MENU_ITEMS            5
#define MENU_INNER           36
#define MENU_WIDTH           (MENU_INNER + 2)
#define MENU_HEIGHT           7
#define MENU_BLOCK           (MENU_HEIGHT + 2)
#define MENU_MARK_AT          1
#define MENU_LABEL_AT         3
#define MENU_VALUE_AT        15
#define MENU_NOTE_AT         24
#define FIELD_MAX             7
#define FIELD_WIDTH           8

#define ITEM_LIQUID           0
#define ITEM_EDGE             1
#define ITEM_BACK             2
#define ITEM_BLOBS            3
#define ITEM_FLOW             4

typedef enum {
    LAMP_RUN   = 0,
    LAMP_LEAVE = 1,
} LampVerdict;

typedef struct Blob {
    double x, y;
    double vx, vy;
    double turn;
    double k;
    double r2;
} Blob;

typedef struct Lamp {
    Blob  *blobs;
    int    count;
    int    ceiling;
    Color  liquid, edge, back;
    int    flow;
    int    rows, cols;
} Lamp;

typedef struct Menu {
    bool open;
    bool editing;
    int  item;
    int  len;
    const char *note;
    char text[FIELD_MAX + 1];
} Menu;

static const char *const kItemNames[MENU_ITEMS] = {
    "liquid", "edge", "background", "blobs", "flow"
};

static const char *const kHintChoose =
    "  up/down or tab choose    left/right or -/+ change";
static const char *const kHintLeave =
    "  enter edit    esc close    ^Q or q leave";

static Lamp      g_lamp;
static Menu      g_menu;
static TextCell *g_frame;
static double   *g_rowterm;
static uint64_t  g_seed;


static uint64_t roll(void)
{
    uint64_t x = g_seed;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_seed = x;
    return x;
}

static double roll_unit(void)
{
    return (double)(int64_t)(roll() >> 40) / 16777216.0;
}

static double roll_range(double lo, double hi)
{
    return lo + (hi - lo) * roll_unit();
}


static double lamp_span(void)
{
    double across = (double)(g_lamp.cols - 1);
    double down   = (double)(g_lamp.rows - 1) * ASPECT;
    return (across < down) ? across : down;
}

static void seed_blob(Blob *b)
{
    double maxx = (double)(g_lamp.cols - 1);
    double maxy = (double)(g_lamp.rows - 1);
    double span = lamp_span();

    b->x = roll_range(0.0, maxx);
    b->y = roll_range(0.0, maxy);

    b->turn = roll_range(maxy * 0.30, maxy * 0.70);

    double period = roll_range(RISE_PERIOD_MIN_S, RISE_PERIOD_MAX_S);
    double omega  = TWO_PI / period;
    b->k = omega * omega;

    b->vy = roll_range(-1.0, 1.0) * omega * maxy * NUDGE_FRACTION;
    b->vx = roll_range(-1.0, 1.0) * (maxx / SIDEWAYS_SECONDS);

    double r = roll_range(span / RADIUS_SMALL_DIV, span / RADIUS_LARGE_DIV);
    if (r < 1.0) r = 1.0;
    b->r2 = r * r;
}

static void reflect(double *p, double *v, double limit)
{
    if (*p < 0.0)   { *p = -*p;             *v = -*v; }
    if (*p > limit) { *p = 2.0 * limit - *p; *v = -*v; }
    if (*p < 0.0)      *p = 0.0;
    else if (*p > limit) *p = limit;
}

static void advance(uint64_t dt_us)
{
    double dt = ((double)(int64_t)dt_us / 1000000.0) *
                ((double)g_lamp.flow / (double)FLOW_DEFAULT);
    if (dt > ADVANCE_MAX_S) dt = ADVANCE_MAX_S;

    double maxx = (double)(g_lamp.cols - 1);
    double maxy = (double)(g_lamp.rows - 1);

    for (int i = 0; i < g_lamp.count; i++) {
        Blob *b = &g_lamp.blobs[i];
        b->vy += -b->k * (b->y - b->turn) * dt;
        b->y  += b->vy * dt;
        b->x  += b->vx * dt;
        reflect(&b->x, &b->vx, maxx);
        reflect(&b->y, &b->vy, maxy);
    }
}


static void cell_paint(double f, TextCell *out)
{
    out->ch = ' ';

    if (f >= CORE_LEVEL) {
        out->bg = g_lamp.liquid;
        out->fg = g_lamp.back;
        return;
    }
    if (f >= EDGE_LEVEL) {
        uint8_t t = (uint8_t)(((f - EDGE_LEVEL) / (CORE_LEVEL - EDGE_LEVEL)) * 255.0);
        out->bg = color_mix(g_lamp.edge, g_lamp.liquid, t);
        out->fg = g_lamp.back;
        return;
    }
    if (f >= RIM_LEVEL) {
        uint8_t t = (uint8_t)(((f - RIM_LEVEL) / (EDGE_LEVEL - RIM_LEVEL)) * 255.0);
        out->bg = color_mix(g_lamp.back, g_lamp.edge, t);
        out->fg = g_lamp.liquid;
        return;
    }
    out->bg = g_lamp.back;
    out->fg = g_lamp.liquid;
}

static void paint_lamp(void)
{
    const int rows  = g_lamp.rows;
    const int cols  = g_lamp.cols;
    const int count = g_lamp.count;

    for (int row = 0; row < rows; row++) {
        for (int i = 0; i < count; i++) {
            double dy = ((double)row - g_lamp.blobs[i].y) * ASPECT;
            g_rowterm[i] = dy * dy + 1.0;
        }

        TextCell *line = &g_frame[row * cols];
        for (int col = 0; col < cols; col++) {
            double f = 0.0;
            for (int i = 0; i < count; i++) {
                double dx = (double)col - g_lamp.blobs[i].x;
                f += g_lamp.blobs[i].r2 / (dx * dx + g_rowterm[i]);
            }
            cell_paint(f, &line[col]);
        }
    }
}

static void put_cell(int row, int col, char ch, Color fg, Color bg)
{
    if (row < 0 || col < 0 || row >= g_lamp.rows || col >= g_lamp.cols) return;
    TextCell *c = &g_frame[row * g_lamp.cols + col];
    c->ch = ch;
    c->fg = fg;
    c->bg = bg;
}

static void put_text(int row, int col, const char *s, Color fg, Color bg)
{
    for (int i = 0; s[i] != '\0'; i++) put_cell(row, col + i, s[i], fg, bg);
}


static int menu_row_origin(void)
{
    int r = (g_lamp.rows - MENU_BLOCK) / 2;
    return (r < 0) ? 0 : r;
}

static int menu_block_width(void)
{
    size_t w = (size_t)MENU_WIDTH;
    size_t choose = strlen(kHintChoose);
    size_t leave  = strlen(kHintLeave);
    if (choose > w) w = choose;
    if (leave  > w) w = leave;
    return (int)w;
}

static int menu_col_origin(void)
{
    int c = (g_lamp.cols - menu_block_width()) / 2;
    return (c < 0) ? 0 : c;
}

static void menu_value(int item, char out[FIELD_WIDTH])
{
    switch (item) {
    case ITEM_LIQUID: color_format(g_lamp.liquid, out); break;
    case ITEM_EDGE:   color_format(g_lamp.edge,   out); break;
    case ITEM_BACK:   color_format(g_lamp.back,   out); break;
    case ITEM_BLOBS:  uint_to_str((unsigned)g_lamp.count, out, FIELD_WIDTH); break;
    default:          uint_to_str((unsigned)g_lamp.flow,  out, FIELD_WIDTH); break;
    }
}

static void draw_menu_row(int row, int col, int item)
{
    bool  selected = (item == g_menu.item);
    Color fg = selected ? MENU_SEL_FG : MENU_FG;
    Color bg = selected ? MENU_SEL_BG : MENU_BG;

    put_cell(row, col, '|', MENU_FG, MENU_BG);
    for (int i = 0; i < MENU_INNER; i++) put_cell(row, col + 1 + i, ' ', fg, bg);
    put_cell(row, col + 1 + MENU_INNER, '|', MENU_FG, MENU_BG);

    put_cell(row, col + 1 + MENU_MARK_AT, selected ? '>' : ' ', fg, bg);
    put_text(row, col + 1 + MENU_LABEL_AT, kItemNames[item], fg, bg);

    int vcol = col + 1 + MENU_VALUE_AT;
    if (g_menu.editing && selected) {
        for (int i = 0; i < FIELD_WIDTH; i++)
            put_cell(row, vcol + i, ' ', MENU_EDIT_FG, MENU_EDIT_BG);
        put_text(row, vcol, g_menu.text, MENU_EDIT_FG, MENU_EDIT_BG);
    } else {
        char value[FIELD_WIDTH];
        menu_value(item, value);
        put_text(row, vcol, value, fg, bg);
    }

    if (selected && g_menu.note != NULL)
        put_text(row, col + 1 + MENU_NOTE_AT, g_menu.note, MENU_NOTE_FG, bg);
}

static void draw_menu(void)
{
    int row = menu_row_origin();
    int col = menu_col_origin();

    put_cell(row, col, '+', MENU_FG, MENU_BG);
    for (int i = 0; i < MENU_INNER; i++)
        put_cell(row, col + 1 + i, '-', MENU_FG, MENU_BG);
    put_cell(row, col + 1 + MENU_INNER, '+', MENU_FG, MENU_BG);
    put_cell(row, col + 2, ' ', MENU_FG, MENU_BG);
    put_text(row, col + 3, "lavalamp", MENU_TITLE_FG, MENU_BG);
    put_cell(row, col + 11, ' ', MENU_FG, MENU_BG);

    for (int i = 0; i < MENU_ITEMS; i++) draw_menu_row(row + 1 + i, col, i);

    int foot = row + 1 + MENU_ITEMS;
    put_cell(foot, col, '+', MENU_FG, MENU_BG);
    for (int i = 0; i < MENU_INNER; i++)
        put_cell(foot, col + 1 + i, '-', MENU_FG, MENU_BG);
    put_cell(foot, col + 1 + MENU_INNER, '+', MENU_FG, MENU_BG);

    put_text(foot + 1, col, kHintChoose, MENU_FG, MENU_BG);
    put_text(foot + 2, col, kHintLeave,  MENU_FG, MENU_BG);
}

static void park_caret(void)
{
    if (!g_menu.open) {
        vga_setcursor((uint8_t)(g_lamp.rows - 1), (uint8_t)(g_lamp.cols - 1));
        return;
    }
    int row = menu_row_origin() + 1 + g_menu.item;
    int col = menu_col_origin() + 1;
    col += g_menu.editing ? (MENU_VALUE_AT + g_menu.len) : MENU_MARK_AT;
    if (row >= g_lamp.rows) row = g_lamp.rows - 1;
    if (col >= g_lamp.cols) col = g_lamp.cols - 1;
    vga_setcursor((uint8_t)row, (uint8_t)col);
}


static bool is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static void begin_edit(void)
{
    if (g_menu.item > ITEM_BACK) {
        g_menu.note = "use -/+";
        return;
    }
    g_menu.editing = true;
    g_menu.note    = NULL;
    g_menu.len     = 0;
    g_menu.text[0] = '\0';
}

static void nudge(int delta)
{
    if (g_menu.item <= ITEM_BACK) {
        g_menu.note = "type it";
        return;
    }
    if (g_menu.item == ITEM_BLOBS) {
        int want = g_lamp.count + delta;
        if (want < 1)              want = 1;
        if (want > g_lamp.ceiling) want = g_lamp.ceiling;
        while (g_lamp.count < want) seed_blob(&g_lamp.blobs[g_lamp.count++]);
        g_lamp.count = want;
        return;
    }
    if (g_menu.item == ITEM_FLOW) {
        int want = g_lamp.flow + delta;
        if (want < FLOW_MIN) want = FLOW_MIN;
        if (want > FLOW_MAX) want = FLOW_MAX;
        g_lamp.flow = want;
    }
}

static void accept_edit(void)
{
    Color parsed;
    if (!color_parse(g_menu.text, &parsed)) {
        g_menu.note = "not a colour";
        return;
    }
    if (g_menu.item == ITEM_LIQUID)    g_lamp.liquid = parsed;
    else if (g_menu.item == ITEM_EDGE) g_lamp.edge   = parsed;
    else                               g_lamp.back   = parsed;
    g_menu.editing = false;
    g_menu.note    = NULL;
}

static LampVerdict edit_key(const kb_event_t *k)
{
    if (k->mods & KB_MOD_EXTENDED) return LAMP_RUN;

    char c = k->ascii;
    if (c == KEY_ESCAPE) {
        g_menu.editing = false;
        g_menu.note    = NULL;
        return LAMP_RUN;
    }
    if (c == KEY_ENTER) {
        accept_edit();
        return LAMP_RUN;
    }
    if (c == KEY_BACKSPACE || c == 0x7F) {
        if (g_menu.len > 0) g_menu.text[--g_menu.len] = '\0';
        g_menu.note = NULL;
        return LAMP_RUN;
    }
    if ((c == '#' || is_hex_digit(c)) && g_menu.len < FIELD_MAX) {
        g_menu.text[g_menu.len++] = c;
        g_menu.text[g_menu.len]   = '\0';
        g_menu.note = NULL;
    }
    return LAMP_RUN;
}

static LampVerdict menu_key(const kb_event_t *k)
{
    g_menu.note = NULL;

    if (k->mods & KB_MOD_EXTENDED) {
        switch (k->scancode) {
        case KEY_UP:    g_menu.item = (g_menu.item + MENU_ITEMS - 1) % MENU_ITEMS; break;
        case KEY_DOWN:  g_menu.item = (g_menu.item + 1) % MENU_ITEMS;              break;
        case KEY_LEFT:  nudge(-1); break;
        case KEY_RIGHT: nudge(+1); break;
        default: break;
        }
        return LAMP_RUN;
    }

    switch (k->ascii) {
    case KEY_ESCAPE: g_menu.open = false;                          break;
    case KEY_TAB:    g_menu.item = (g_menu.item + 1) % MENU_ITEMS; break;
    case KEY_ENTER:  begin_edit();                                 break;
    case '-':        nudge(-1);                                    break;
    case '+':        nudge(+1);                                    break;
    default: break;
    }
    return LAMP_RUN;
}

static LampVerdict handle_key(const kb_event_t *k)
{
    if ((k->mods & KB_MOD_CTRL) && (k->ascii == 'q' || k->ascii == 'Q'))
        return LAMP_LEAVE;

    if (g_menu.editing) return edit_key(k);

    if (k->ascii == 'q' || k->ascii == 'Q') return LAMP_LEAVE;

    if (!g_menu.open) {
        if (k->ascii == KEY_ESCAPE) g_menu.open = true;
        return LAMP_RUN;
    }
    return menu_key(k);
}


static int lamp_ceiling(int rows, int cols)
{
    int c = (rows * cols) / CELLS_PER_BLOB;
    return (c < BLOB_CEILING_MIN) ? BLOB_CEILING_MIN : c;
}

static void release_lamp(void)
{
    free(g_frame);      g_frame      = NULL;
    free(g_rowterm);    g_rowterm    = NULL;
    free(g_lamp.blobs); g_lamp.blobs = NULL;
}

static int lamp_setup(void)
{
    vga_dimensions_t dim;
    if (vga_getdimensions(&dim) != 0) {
        println("This machine did not say how big its screen is.");
        return 1;
    }
    if (dim.rows == 0 || dim.cols == 0) {
        println("This machine has no console to paint on.");
        return 1;
    }

    g_lamp.rows    = dim.rows;
    g_lamp.cols    = dim.cols;
    g_lamp.ceiling = lamp_ceiling(g_lamp.rows, g_lamp.cols);
    g_lamp.count   = (BLOB_DEFAULT < g_lamp.ceiling) ? BLOB_DEFAULT : g_lamp.ceiling;
    g_lamp.flow    = FLOW_DEFAULT;
    g_lamp.liquid  = DEFAULT_LIQUID;
    g_lamp.edge    = DEFAULT_EDGE;
    g_lamp.back    = DEFAULT_BACK;

    g_frame      = (TextCell *)calloc((size_t)g_lamp.rows * (size_t)g_lamp.cols,
                                      sizeof(TextCell));
    g_lamp.blobs = (Blob *)malloc((size_t)g_lamp.ceiling * sizeof(Blob));
    g_rowterm    = (double *)malloc((size_t)g_lamp.ceiling * sizeof(double));
    if (!g_frame || !g_lamp.blobs || !g_rowterm) {
        release_lamp();
        println("There is not enough memory for a lamp the size of this screen.");
        return 1;
    }

    g_seed  = cpu_rdtsc() * 0x9E3779B97F4A7C15ull;
    g_seed |= 1u;
    for (int i = 0; i < g_lamp.count; i++) seed_blob(&g_lamp.blobs[i]);
    return 0;
}

static int run_lamp(TouchTag ear, uint32_t *frames)
{
    uint64_t last_us = clock_uptime_us();
    uint64_t next_us = last_us;

    for (;;) {
        uint64_t now = clock_uptime_us();
        if (now >= next_us) {
            uint64_t dt_us = now - last_us;
            last_us = now;

            advance(dt_us);
            paint_lamp();
            if (g_menu.open) draw_menu();

            park_caret();
            if (vga_paint(0, 0, (uint8_t)g_lamp.rows, (uint8_t)g_lamp.cols,
                          g_frame) != 0)
                return 1;
            (*frames)++;

            next_us = clock_uptime_us() + FRAME_US;
            continue;
        }

        uint32_t left_ms = (uint32_t)((next_us - now) / 1000u);
        if (left_ms == 0) left_ms = 1;

        Touch t;
        int rc = touch_await(ear, &t, left_ms);
        if (rc == 0 && t.payload_len >= sizeof(kb_event_t)) {
            kb_event_t k;
            memcpy(&k, t.payload, sizeof(k));
            if (handle_key(&k) == LAMP_LEAVE) return 0;
        } else if (rc != 0 && rc != -ERR_TIMEOUT) {
            return 0;
        }
    }
}

static void say_the_tally(uint32_t frames, int code)
{
    console_unlisten();
    vga_clear_rgb(COLOR_LIGHT_GRAY, COLOR_BLACK);
    vga_setcursor(0, 0);

    char liquid[8], back[8];
    color_format(g_lamp.liquid, liquid);
    color_format(g_lamp.back, back);

    if (code != 0) println("The screen stopped taking frames.");
    printf("[LAVALAMP] %u frames - liquid %s over %s\n",
           (unsigned)frames, liquid, back);
}

int main(void)
{
    int argc = (int)luggage_word_count();
    if (argc > 1) {
        println("Usage: lavalamp");
        exit(1);
        return 1;
    }

    if (lamp_setup() != 0) {
        exit(1);
        return 1;
    }

    TouchTag ear = console_listen();
    if (ear == TOUCH_TAG_INVALID) {
        release_lamp();
        println("This machine has no way to hear a key.");
        exit(1);
        return 1;
    }

    uint32_t frames  = 0;
    int      verdict = run_lamp(ear, &frames);

    say_the_tally(frames, verdict);
    release_lamp();
    exit(verdict);
    return verdict;
}