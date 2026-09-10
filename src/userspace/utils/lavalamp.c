/* lavalamp — a lamp of hot liquid on the console's grid, coloured by hand.
 *
 * The screen is a grid of coloured cells and nothing finer, so a picture on it
 * has to be made out of colour rather than out of shape. Metaballs are the one
 * classic that suits that exactly: the field is a sum of squared radii over
 * squared distances, every cell is a space with a background, and the blobs
 * merge and part on their own. No square root is taken and none is needed —
 * which matters here, because a cabin has no libm and never will.
 *
 * The colours are the point. #rrggbb is how this system spells a colour
 * everywhere, and this is the first program where a person spells one back:
 * ESC opens a menu over the running lamp, and its three colour rows are fields
 * to type into — read by color_parse, shown by color_format.
 *
 * The picture leans on the kernel clearing the Attribute Controller's blink
 * enable at VGA init: on a BIOS/VGA-text console the background nibble's high
 * bit used to be the BLINK bit, so a light-family background — which #ff3a6b
 * is — made the blob cores flash instead of glow, and putting blink back
 * would cost this lamp the very cells it is built out of.
 *
 *   lavalamp            -> the lamp, whole screen, until it is told to leave
 *   ESC                 -> the menu opens and closes; the lamp keeps flowing
 *   up/down, tab        -> choose a row
 *   enter               -> type a colour into the row under the marker
 *   left/right, -/+     -> more blobs or fewer, faster flow or slower
 *   ^Q, q               -> leave, and say how many frames it drew
 *
 * A serial console hands every key over as a bare byte: no arrows, no Ctrl,
 * no modifiers at all. That is why tab, '-', '+' and a plain q stand beside
 * the keys a framebuffer user would reach for. Take them away and this becomes
 * a program a person on a serial line can enter and cannot leave.
 *
 * Do not paint this with vga_putchar_at. One frame is thousands of cells; as
 * ops they would be thousands of Manifest entries AND thousands of lines in
 * the log ring the serial line reads, twenty-five times a second. vga_paint is
 * one op and stays out of the ring — that is exactly what it is for.
 */
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

/* A cell is 8x16 pixels, so a distance measured down the screen counts double
 * against one measured across it. Without this every blob is an egg standing
 * on its end. */
#define ASPECT              2.0
#define TWO_PI              6.28318530717958647692

/* The three levels the field is read against.
 *
 * One blob's field at distance d is r*r / (d*d + 1), so at d = r it is
 * r*r/(r*r+1) — a hair under 1 for any blob worth looking at. EDGE_LEVEL 1.0
 * therefore puts the blob's rim exactly at its nominal radius, which is what
 * makes the radius a number that means something.
 *
 * The other two follow from the same formula. A level L sits at
 * d = r / sqrt(L), so the AREA inside L is 1/L of the area inside 1.0:
 * CORE_LEVEL 2.5 gives a core covering two fifths of the blob — enough to read
 * as a body of liquid rather than as a dot. RIM_LEVEL 0.55 reaches out to
 * about 1.35 r, so the soft edge is a third of a radius wide: visible as a
 * gradient, too narrow to be mistaken for the liquid itself.
 */
#define RIM_LEVEL           0.55
#define EDGE_LEVEL          1.00
#define CORE_LEVEL          2.50

/* A blob is sized against the SMALLER side of the lamp, measured in column
 * units — a screen 240 wide and 12 tall must not get blobs taller than it is. */
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

#define FRAME_US            40000u          /* 25 frames a second; 10 ticks */

#define DEFAULT_LIQUID      COLOR_RGB(0xFF, 0x3A, 0x6B)
#define DEFAULT_EDGE        COLOR_RGB(0x7A, 0x10, 0x40)
#define DEFAULT_BACK        COLOR_RGB(0x10, 0x08, 0x20)

/* The menu is chrome, not lamp: its colours are its own, so a person who has
 * just typed #100820 over #100820 can still read the row that says so. */
#define MENU_FG             COLOR_LIGHT_GRAY
#define MENU_BG             COLOR_RGB(0x10, 0x12, 0x18)
#define MENU_SEL_FG         COLOR_WHITE
#define MENU_SEL_BG         COLOR_RGB(0x28, 0x30, 0x40)
#define MENU_TITLE_FG       COLOR_AMBER
#define MENU_EDIT_FG        COLOR_WHITE
#define MENU_EDIT_BG        COLOR_RGB(0x48, 0x2A, 0x10)
#define MENU_NOTE_FG        COLOR_RED

#define MENU_ITEMS            5
#define MENU_INNER           36             /* between the two '|' columns */
#define MENU_WIDTH           (MENU_INNER + 2)
#define MENU_HEIGHT           7             /* border, five rows, border */
#define MENU_BLOCK           (MENU_HEIGHT + 2)   /* the two hint lines below */
#define MENU_MARK_AT          1
#define MENU_LABEL_AT         3
#define MENU_VALUE_AT        15
#define MENU_NOTE_AT         24
#define FIELD_MAX             7             /* "#rrggbb" and not a byte more */
#define FIELD_WIDTH           8             /* the field plus the caret's cell */

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
    double x, y;        /* where its centre is: x in columns, y in rows */
    double vx, vy;      /* cells a second */
    double turn;        /* the row this one turns around — its own, not shared */
    double k;           /* how hard it is pushed back to `turn`, per second^2 */
    double r2;          /* radius squared, in column units */
} Blob;

typedef struct Lamp {
    Blob  *blobs;
    int    count;
    int    ceiling;
    Color  liquid, edge, back;
    int    flow;                /* FLOW_MIN..FLOW_MAX, scales the time step */
    int    rows, cols;
} Lamp;

typedef struct Menu {
    bool open;
    bool editing;
    int  item;
    int  len;
    /* What the chosen row has to say about the last key it was handed, or NULL
     * for nothing. This was a bool meaning "the last enter did not parse", and
     * only a colour row could ever set it — so enter on a number row and -/+
     * on a colour row went nowhere without a word, against a hint line that
     * promises both keys to every row. */
    const char *note;
    char text[FIELD_MAX + 1];
} Menu;

static const char *const kItemNames[MENU_ITEMS] = {
    "liquid", "edge", "background", "blobs", "flow"
};

/* The two lines drawn under the panel. Named here because the block is centred
 * on whichever drawn line is widest, and these are it: 51 and 42 columns
 * against the panel's 38. Centred on MENU_WIDTH instead, thirteen columns of
 * hint hung off the right edge of an 80-column console and the whole block sat
 * right of centre. */
static const char *const kHintChoose =
    "  up/down or tab choose    left/right or -/+ change";
static const char *const kHintLeave =
    "  enter edit    esc close    ^Q or q leave";

static Lamp      g_lamp;
static Menu      g_menu;
static TextCell *g_frame;
/* One entry per blob, refilled once per row: the vertical half of the field is
 * the same for every cell on a row, and hoisting it out of the column loop is
 * the difference between two multiplies a cell and two multiplies a row. */
static double   *g_rowterm;
static uint64_t  g_seed;

/* ===========================================================================
 * Numbers out of thin air
 * =========================================================================== */

static uint64_t roll(void)
{
    uint64_t x = g_seed;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_seed = x;
    return x;
}

/* The top 24 bits, as 0.0 .. 1.0. Taken through int64_t on purpose: unsigned
 * 64-to-double is a branch sequence on x86_64, signed is one instruction, and
 * 24 bits can never be negative. */
static double roll_unit(void)
{
    return (double)(int64_t)(roll() >> 40) / 16777216.0;
}

static double roll_range(double lo, double hi)
{
    return lo + (hi - lo) * roll_unit();
}

/* ===========================================================================
 * The model
 * =========================================================================== */

/* The lamp's smaller side, in column units — the dimension a blob must fit
 * inside whichever way the screen happens to be shaped. */
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

    /* Its own turning point in the middle band, and its own period. Two blobs
     * that shared either would rise and sink together, and a lamp whose blobs
     * move in step is a clock face, not a lamp. */
    b->turn = roll_range(maxy * 0.30, maxy * 0.70);

    double period = roll_range(RISE_PERIOD_MIN_S, RISE_PERIOD_MAX_S);
    double omega  = TWO_PI / period;
    b->k = omega * omega;

    /* A nudge, not a launch: the excursion a blob ends up with is mostly the
     * distance it starts from its turning point, so the half-period IS the
     * time it takes to cross — twelve to twenty seconds at flow 5. */
    b->vy = roll_range(-1.0, 1.0) * omega * maxy * NUDGE_FRACTION;
    b->vx = roll_range(-1.0, 1.0) * (maxx / SIDEWAYS_SECONDS);

    double r = roll_range(span / RADIUS_SMALL_DIV, span / RADIUS_LARGE_DIV);
    /* A lamp a couple of cells tall would otherwise size its blobs down to
     * nothing and show a screenful of background: below one cell there is no
     * smaller blob to draw, so one cell is where the scaling stops. */
    if (r < 1.0) r = 1.0;
    b->r2 = r * r;
}

/* Elastic: the wall gives nothing back and takes nothing away. The clamp after
 * the two reflections is the guard for a step so long that one bounce is not
 * enough — ADVANCE_MAX_S makes that unreachable, and a lamp that survives an
 * unreachable case costs two comparisons. */
static void reflect(double *p, double *v, double limit)
{
    if (*p < 0.0)   { *p = -*p;             *v = -*v; }
    if (*p > limit) { *p = 2.0 * limit - *p; *v = -*v; }
    if (*p < 0.0)      *p = 0.0;
    else if (*p > limit) *p = limit;
}

static void advance(uint64_t dt_us)
{
    /* MEASURED time, scaled by flow — never a fixed step per frame. A slow
     * machine draws fewer frames and moves the lamp further in each one, so
     * the lamp looks the same on both. A stall longer than ADVANCE_MAX_S is
     * capped rather than believed: a blob must not teleport across the glass
     * because the machine went away for a second. */
    double dt = ((double)(int64_t)dt_us / 1000000.0) *
                ((double)g_lamp.flow / (double)FLOW_DEFAULT);
    if (dt > ADVANCE_MAX_S) dt = ADVANCE_MAX_S;

    double maxx = (double)(g_lamp.cols - 1);
    double maxy = (double)(g_lamp.rows - 1);

    for (int i = 0; i < g_lamp.count; i++) {
        Blob *b = &g_lamp.blobs[i];
        /* Buoyancy: pushed up while it is below its turning point and down
         * while it is above it, by a force proportional to how far it is
         * from it. That makes the rise and the sink take the same time
         * whatever the excursion — a lamp, not a bouncing ball. */
        b->vy += -b->k * (b->y - b->turn) * dt;
        b->y  += b->vy * dt;
        b->x  += b->vx * dt;
        reflect(&b->x, &b->vx, maxx);
        reflect(&b->y, &b->vy, maxy);
    }
}

/* ===========================================================================
 * The picture
 * =========================================================================== */

static void cell_paint(double f, TextCell *out)
{
    out->ch = ' ';                       /* colour is the whole picture */

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
            /* The field's +1 lives here rather than in the column loop: it is
             * a constant of the denominator, and folding it in is exact. */
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

/* ===========================================================================
 * The menu
 * =========================================================================== */

static int menu_row_origin(void)
{
    int r = (g_lamp.rows - MENU_BLOCK) / 2;
    return (r < 0) ? 0 : r;
}

/* Measured, never written down twice: a hint line that grows a word moves the
 * block with it. menu_row_origin already counts the hint ROWS in its height;
 * this is the same accounting across. */
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

/* FIELD_WIDTH bytes, always — the size is dropped from the signature rather
 * than checked in it. It used to take an out_size that two of the five cases
 * honoured and color_format ignored, writing its eight bytes whatever the
 * bound said; a promise kept three fifths of the time is worse than no promise
 * at all. The array parameter states the size where the compiler sees it, and
 * it matches color_format's own `char out[8]`. */
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

    /* The marker is a character AND a background. The character alone would be
     * the only mark on a console that quantises colour hard. */
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

    /* Twelve columns between MENU_NOTE_AT and the right border, which is what
     * the longest note ("not a colour") needs to the character. */
    if (selected && g_menu.note != NULL)
        put_text(row, col + 1 + MENU_NOTE_AT, g_menu.note, MENU_NOTE_FG, bg);
}

static void draw_menu(void)
{
    int row = menu_row_origin();
    int col = menu_col_origin();

    /* "+- lavalamp " and then rule to the far corner. The whole row is ruled
     * first and the name laid over it, so no cell of the border is left
     * unwritten with the lamp still showing through. ASCII only: the font is
     * 8x16 ASCII and any other byte lands as '?'. */
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

/* Where the caret belongs. It is drawn wherever the kernel cursor stands and
 * there is no way to hide it, so it is put somewhere it means something: on
 * the character being typed, on the marker of the chosen row, or out of the
 * way in the corner while the lamp has the screen to itself. */
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

/* ===========================================================================
 * The keys
 * =========================================================================== */

static bool is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static void begin_edit(void)
{
    if (g_menu.item > ITEM_BACK) {
        /* A number row has no field to open. It used to return in silence, so
         * enter on "blobs" read as a dead key next to a hint line that says
         * enter edits. */
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
        /* There is no colour one step past #ff3a6b, so -/+ and the arrows have
         * nothing to walk here. They used to be swallowed whole; now the row
         * points at the key that does work. */
        g_menu.note = "type it";
        return;
    }
    if (g_menu.item == ITEM_BLOBS) {
        int want = g_lamp.count + delta;
        if (want < 1)              want = 1;
        if (want > g_lamp.ceiling) want = g_lamp.ceiling;
        /* One more is a new blob somewhere random; one fewer is the last one
         * dropped, and its slot keeps its numbers until something seeds over
         * them — so counting back up does not resurrect the same blob. */
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
        /* The field stays open with what was typed still in it: a colour that
         * is one digit short is a colour that is one digit from being right,
         * and throwing it away would make the person type all seven again. */
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
    if (k->mods & KB_MOD_EXTENDED) return LAMP_RUN;   /* arrows do nothing here */

    char c = k->ascii;
    if (c == KEY_ESCAPE) {                            /* cancel: the old value stands */
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
    /* A note answers the LAST key, so every key clears it first; the two calls
     * below that have something to say set it again. Left standing, it would
     * follow the marker onto a row that never earned it. */
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

    /* A bare q, for the console that cannot deliver a modifier at all. It is
     * unreachable while a field is open, where q is not a hex digit anyway. */
    if (k->ascii == 'q' || k->ascii == 'Q') return LAMP_LEAVE;

    if (!g_menu.open) {
        if (k->ascii == KEY_ESCAPE) g_menu.open = true;
        return LAMP_RUN;
    }
    return menu_key(k);
}

/* ===========================================================================
 * Setting up and leaving
 * =========================================================================== */

static int lamp_ceiling(int rows, int cols)
{
    int c = (rows * cols) / CELLS_PER_BLOB;
    return (c < BLOB_CEILING_MIN) ? BLOB_CEILING_MIN : c;
}

/* The three things the lamp holds while it runs. Released together, and only
 * once — every refusal below leaves through here so no path can forget one. */
static void release_lamp(void)
{
    free(g_frame);      g_frame      = NULL;
    free(g_rowterm);    g_rowterm    = NULL;
    free(g_lamp.blobs); g_lamp.blobs = NULL;
}

/* Everything the lamp needs from the machine, asked for once. Returns 0, or 1
 * having already said which refusal it was. */
static int lamp_setup(void)
{
    vga_dimensions_t dim;
    if (vga_getdimensions(&dim) != 0) {
        println("This machine did not say how big its screen is.");
        return 1;
    }
    if (dim.rows == 0 || dim.cols == 0) {
        /* The kernel clamps the geometry at 255 now rather than truncating it,
         * so a zero has exactly one meaning left: there is no console. */
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

    /* The frame is the screen's size, which is the screen's business — 762 KB
     * on the widest console this ABI can address (255 x 255 cells of twelve
     * bytes; the 193 KB this once claimed is a 240x67 screen, not the widest
     * one), and so it is heap and never the 64 KB user stack. calloc, so the
     * three pad bytes of every cell are zero once and stay zero: the paint op
     * copies them onto the wire as they lie. */
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
    g_seed |= 1u;                          /* xorshift dies on a zero state */
    for (int i = 0; i < g_lamp.count; i++) seed_blob(&g_lamp.blobs[i]);
    return 0;
}

/* The lamp, until it is told to leave. Returns 0, or 1 if the screen stopped
 * taking frames — which is not something to keep trying at 25 a second. */
static int run_lamp(TouchTag ear, uint32_t *frames)
{
    uint64_t last_us = clock_uptime_us();
    uint64_t next_us = last_us;            /* the first frame is due at once */

    for (;;) {
        uint64_t now = clock_uptime_us();
        if (now >= next_us) {
            uint64_t dt_us = now - last_us;
            last_us = now;

            advance(dt_us);
            paint_lamp();
            if (g_menu.open) draw_menu();

            /* Parked BEFORE the frame: the frame is then the last thing to
             * reach the glass and already carries the caret where it belongs.
             * Painted first, the paint's own commit draws the caret wherever
             * the cursor happened to stand — inside the picture, or on the
             * character typed in the menu one keystroke ago. */
            park_caret();
            if (vga_paint(0, 0, (uint8_t)g_lamp.rows, (uint8_t)g_lamp.cols,
                          g_frame) != 0)
                return 1;
            (*frames)++;

            /* The next deadline is taken from the clock AFTER the painting,
             * not before it. On a screen wide enough that one frame costs more
             * than FRAME_US, a deadline measured before the paint is already
             * past when the paint ends — the loop would paint again without
             * ever asking for a key, and the program would go deaf exactly
             * where it is slowest. Measured time still drives the motion, so a
             * machine that cannot hold 25 frames draws fewer of them and the
             * lamp flows at the same speed. */
            next_us = clock_uptime_us() + FRAME_US;
            continue;
        }

        /* Parked in the kernel with a timer until the frame is due. The core
         * goes idle meanwhile: no spin, no yield loop, no throttle. */
        uint32_t left_ms = (uint32_t)((next_us - now) / 1000u);
        if (left_ms == 0) left_ms = 1;

        Touch t;
        int rc = touch_await(ear, &t, left_ms);
        if (rc == 0 && t.payload_len >= sizeof(kb_event_t)) {
            kb_event_t k;
            memcpy(&k, t.payload, sizeof(k));
            if (handle_key(&k) == LAMP_LEAVE) return 0;
        } else if (rc != 0 && rc != -ERR_TIMEOUT) {
            /* -ERR_TIMEOUT is the ONLY refusal that means the frame is due:
             * it is the only one that spent the wait. Bad arguments and a
             * submit that did not go come back at once, and treating every
             * non-zero answer as a due frame re-entered this loop instantly
             * and spun the core this program was written never to spin on,
             * deaf to every key. A picture nobody can stop is worse than a
             * short one — the ear is gone, so leave the way q leaves. */
            return 0;
        }
    }
}

/* Give the screen back, THEN speak. Printed text travels a Brook lane to the
 * display daemon and lands whenever the daemon next runs — say anything while
 * the lamp still owns the glass and it appears on top of a frame, at a moment
 * nobody chose. */
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

    /* The ear is taken last, so no refusal above has to give it back. */
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
