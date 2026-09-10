/* playtime — a waterfall of colour: a screenful of #rrggbb that nothing rounds
 * on the way down, laid by ONE syscall.
 *
 * The owner asked for this one for fun, and it earns its keep twice over. What
 * it is NOT — this header claimed it for a while — is a proof that all
 * twenty-four bits reach the glass: every cell is color_wheel(hue, 255, 255),
 * and at full saturation one channel is always 0 and one always 255. That is
 * the saturated rim of the colour cube, 1536 colours, not sixteen million.
 * What it does prove is that those 1536 arrive exactly as they were spelled,
 * and that a whole screenful of distinct ones costs a single op.
 *
 * Every cell is a space with a background, so the picture is colour and no
 * glyph at all; each column pours at its own speed, the hue climbs with the row
 * so a column reads as a ribbon, and the whole field turns slowly so the same
 * screenful never comes back. Any key stops it — the screen says so nowhere,
 * because the screen is the picture.
 *
 *   playtime           -> pours until a key is pressed
 *
 * Two things here must not be undone. Nothing is printed between the first
 * paint and the final clear: printed text travels the display daemon's lane
 * and would land on top of the picture at a moment nobody chose. And a frame
 * is laid down by ONE vga_paint, never by a loop of vga_putchar_at — a
 * per-cell op would also put sixteen thousand characters a frame into the log
 * ring the serial line reads, and bury everything the machine actually said.
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
#include "box/error.h"

#define FRAME_US       40000u    /* 25 frames a second — ten kernel ticks exactly */

#define HUE_TURN       1536u     /* one full turn of the wheel (box/color.h) */
#define ROW_STEP       48u       /* hue per row: 48 rows carry 2304, one and a half turns */
#define ROW_DOWN       (HUE_TURN - ROW_STEP)   /* the same step, taken downward */

#define PHASE_FRAC     256u      /* a phase is hue x 256 */
#define PHASE_TURN     (HUE_TURN * PHASE_FRAC)

#define SPEED_SLOW_HZ  300u      /* the laziest column: 300/48 = 6.2 rows a second */
#define SPEED_FAST_HZ  900u      /* the quickest: 18.8 rows a second, three times the laziest */
#define DRIFT_HZ       40u       /* the field itself turns once every 38 seconds */

static uint64_t g_rng;
static uint32_t g_drift;         /* where the whole field has turned to, in phase units */

static vga_dimensions_t g_dim;
static TextCell *g_frame;

/* One entry per column OF THIS SCREEN, taken from the heap once the screen has
 * said how wide it is. These were two 256-entry arrays in BSS — a fixed ceiling
 * standing in for a number the program already holds. Both are phase units —
 * hue x PHASE_FRAC — and speed is per second. */
static uint32_t *s_phase;
static uint32_t *s_speed;

/* xorshift64: four lines, no state to allocate, and it never repeats inside a
 * run this short. There is no rand() in a cabin and there should not be. */
static uint64_t rng_next(void)
{
    uint64_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng = x;
    return x;
}

/* ‼ NEIGHBOURS MUST START TOGETHER, or this is not water.
 *
 * The first shape of this drew each column's phase and speed independently.
 * Every number in it was right and the picture was wrong: with neighbours
 * unrelated, a screenful is vertical stripes of unrelated colour — a barcode
 * that happens to move. What makes a waterfall read as one is that the columns
 * beside each other are ALMOST the same and only slowly differ, so the eye
 * follows a band of colour across the screen and watches it be pulled apart.
 *
 * So both are laid down as a bounded random walk along the screen: each column
 * takes its neighbour's value and steps a little way from it. The phase also
 * carries a steady ramp, which lays about a turn and a quarter of the wheel
 * across the width and gives the eye its bands; the speeds stay inside their
 * range and drift, which shears those bands into folds as they pour. */
#define PHASE_RAMP     12u       /* hue per column: ~1.25 turns across 160 columns */
#define PHASE_WALK     (6u * PHASE_FRAC)     /* +-6 hue of texture on the ramp */
#define SPEED_WALK     (24u * PHASE_FRAC)    /* +-24 hue/s between neighbours */

static uint32_t walk_step(uint32_t span)
{
    return (uint32_t)(rng_next() % (2u * span + 1u)) - span;   /* -span .. +span */
}

static void seed_columns(unsigned cols)
{
    /* The counter is the only thing on the machine that differs between two
     * boots of the same image; the low bit is forced so xorshift, which dies
     * on zero, cannot start dead. */
    g_rng = cpu_rdtsc() | 1u;

    uint32_t phase = (uint32_t)(rng_next() % PHASE_TURN);
    uint32_t speed = (SPEED_SLOW_HZ + SPEED_FAST_HZ) / 2u * PHASE_FRAC;

    const uint32_t slow = SPEED_SLOW_HZ * PHASE_FRAC;
    const uint32_t fast = SPEED_FAST_HZ * PHASE_FRAC;

    for (unsigned c = 0; c < cols; c++) {
        s_phase[c] = phase;
        s_speed[c] = speed;

        /* Unsigned throughout: walk_step returns its negative steps as the
         * wrap-around they are, and the modulo puts the sum back on the wheel.
         * The speed has no wheel to come back on, so it is held inside its
         * range by reflection — a column that would have gone slower than the
         * slowest turns around and speeds up again. */
        phase = (phase + PHASE_RAMP * PHASE_FRAC + walk_step(PHASE_WALK)) % PHASE_TURN;

        uint32_t next = speed + walk_step(SPEED_WALK);
        if (next < slow || next > fast) next = speed - (next - speed);
        speed = (next < slow || next > fast) ? speed : next;
    }
}

/* Move the picture by the time that actually passed, not by one notch per
 * frame: a machine that manages ten frames a second then shows the same pour
 * as one that manages twenty-five, only in coarser steps. */
static void advance(unsigned cols, uint64_t dt_us)
{
    for (unsigned c = 0; c < cols; c++) {
        uint32_t step = (uint32_t)(((uint64_t)s_speed[c] * dt_us) / 1000000u);
        s_phase[c] = (s_phase[c] + step) % PHASE_TURN;
    }

    /* Why the phase carries a x256 fraction at all: a 40 ms step of the drift
     * is 1.6 hue, and in whole hue that truncates to 1 — a field turning 37%
     * slower than the constant above says. In phase units it is 409 of 409.6,
     * and the error is a sixth of a percent. */
    uint32_t drift_step = (uint32_t)(((uint64_t)DRIFT_HZ * PHASE_FRAC * dt_us) / 1000000u);
    g_drift = (g_drift + drift_step) % PHASE_TURN;
}

/* Fill the frame. Only fg and bg are written — ch and pad were set once, when
 * the frame was allocated, and every cell of every frame is a space. Two
 * stores a cell instead of four, on the sixteen thousand cells a 240x67
 * console has. */
static void compose(TextCell *frame, unsigned rows, unsigned cols)
{
    for (unsigned r = 0; r < rows; r++) {
        /* ROW_DOWN is ROW_STEP counted backwards around the wheel. A colour
         * sits where (phase - r * ROW_STEP) is constant, so as the phase grows
         * the row holding that colour grows with it and the ribbon slides
         * DOWN. Written as an addition so the arithmetic stays unsigned. */
        uint32_t row_hue = ((uint32_t)r * ROW_DOWN) % HUE_TURN;

        TextCell *line = &frame[(size_t)r * cols];

        for (unsigned c = 0; c < cols; c++) {
            uint32_t hue = ((s_phase[c] + g_drift) / PHASE_FRAC + row_hue) % HUE_TURN;

            /* Full saturation and full value: this is a rainbow, not a
             * pastel. The foreground is given the same colour as the
             * background so the cell is one flat colour whichever backend
             * draws it — a GOP framebuffer paints the space's background,
             * a VGA text cell quantises both halves of the pair. */
            Color k = color_wheel((uint16_t)hue, 255, 255);
            line[c].fg = k;
            line[c].bg = k;
        }
    }
}

/* Where the caret belongs. It is drawn wherever the kernel cursor stands and
 * there is no way to hide it, so it is parked in the far corner rather than
 * left blinking in the middle of the picture. vga_paint never moves it — but
 * a kernel line does, and so does the display daemon rendering another lane,
 * and this used to be called once before the loop: one such line and the caret
 * blinked inside the picture for the rest of the run. */
static void park_caret(void)
{
    vga_setcursor((uint8_t)(g_dim.rows - 1), (uint8_t)(g_dim.cols - 1));
}

/* The three things the pour holds while it runs, released together so no
 * refusal below can forget one. */
static void release_pour(void)
{
    free(g_frame); g_frame = NULL;
    free(s_phase); s_phase = NULL;
    free(s_speed); s_speed = NULL;
}

/* Everything the pour needs from the machine, asked for once. Returns 0, or 1
 * having already said which refusal it was. */
static int pour_setup(void)
{
    if (vga_getdimensions(&g_dim) != 0) {
        println("The screen will not say how big it is.");
        return 1;
    }

    /* The kernel op CLAMPS a screen wider than the ABI can name: more than 255
     * columns answers 255, and the part that can be named is the part painted.
     * It used to truncate to a byte instead, so 320 columns came back as 64 and
     * this test — then dressed up as "too many cells to count" — passed on a
     * screen it was supposed to refuse. A zero has one meaning left. */
    if (g_dim.rows == 0 || g_dim.cols == 0) {
        println("This machine has no console to paint on.");
        return 1;
    }

    size_t cells = (size_t)g_dim.rows * (size_t)g_dim.cols;
    g_frame = (TextCell *)malloc(cells * sizeof(TextCell));
    s_phase = (uint32_t *)malloc((size_t)g_dim.cols * sizeof(uint32_t));
    s_speed = (uint32_t *)malloc((size_t)g_dim.cols * sizeof(uint32_t));
    if (!g_frame || !s_phase || !s_speed) {
        release_pour();
        println("There is not enough memory for one frame of this screen.");
        return 1;
    }

    memset(g_frame, 0, cells * sizeof(TextCell));
    for (size_t i = 0; i < cells; i++) {
        g_frame[i].ch = ' ';
    }

    seed_columns(g_dim.cols);
    return 0;
}

/* The pour, until a key stops it. Returns 0, or 1 if the screen stopped taking
 * frames — which is not something to keep trying at 25 a second. */
static int run_pour(TouchTag ear, uint64_t *frames)
{
    uint64_t last_us = clock_uptime_us();
    uint64_t next_us = last_us;      /* the first frame is due at once */

    for (;;) {
        uint64_t now = clock_uptime_us();

        if (now >= next_us) {
            uint64_t dt_us = now - last_us;
            last_us = now;

            advance(g_dim.cols, dt_us);
            compose(g_frame, g_dim.rows, g_dim.cols);

            /* A screen that refuses the frame will refuse the next one too,
             * and there is nothing to watch — leave, and let the tally say
             * why: this used to return through the same door as a keypress
             * and the only word about it was "0 frames in 0 ms". */
            /* Parked BEFORE the frame, so the frame is the last thing to
             * reach the glass and it carries the caret already in the corner.
             * The other way round, the paint's own commit draws the caret at
             * whatever position the last line of somebody else's output left,
             * and it stands inside the picture until the next op lands. */
            park_caret();
            if (vga_paint(0, 0, g_dim.rows, g_dim.cols, g_frame) != 0) return 1;
            (*frames)++;

            /* The next deadline is taken AFTER the paint, not from the `now`
             * above. A console whose framebuffer is uncached can spend longer
             * than FRAME_US laying a frame down, and a deadline already in the
             * past would send this loop straight back to painting — the key
             * that stops it would never be listened for at all. */
            next_us = clock_uptime_us() + FRAME_US;
            continue;
        }

        /* The wait is a kernel park with a timer, so the core goes idle until
         * either a key arrives or the frame comes due. No spin, no yield loop,
         * no throttle: the deadline IS the pacing. */
        uint32_t left_ms = (uint32_t)((next_us - now) / 1000u);
        if (left_ms == 0) left_ms = 1;

        Touch t;
        int rc = touch_await(ear, &t, left_ms);
        if (rc == 0) {
            /* Any key at all, and which one it was does not matter — not even
             * over a serial line, where every byte arrives as a key of its
             * own with no scancode and no modifiers. */
            if (t.payload_len >= sizeof(kb_event_t)) return 0;
        } else if (rc != -ERR_TIMEOUT) {
            /* The ear answered neither a key nor "not yet": something has
             * taken it, and a picture nobody can stop is worse than a short
             * one. */
            return 0;
        }
    }
}

/* Give the screen back, THEN speak — printed text travels the display daemon's
 * lane and would land on top of a frame at a moment nobody chose. The
 * [PLAYTIME] line is spoken whichever way the pour ended: an oracle greps for
 * it and for the two numbers it carries. */
static void say_the_tally(uint64_t frames, uint64_t ran_ms, int code)
{
    console_unlisten();
    vga_clear_rgb(COLOR_LIGHT_GRAY, COLOR_BLACK);
    vga_setcursor(0, 0);

    /* Now, and not one line earlier, it is safe to speak. */
    if (code != 0) println("The screen stopped taking frames.");

    printf("[PLAYTIME] %u frames in %u ms\n",
           (unsigned)frames, (unsigned)ran_ms);
}

int main(void)
{
    if (luggage_word_count() > 1) {
        println("Usage: playtime");
        exit(1);
        return 1;
    }

    if (pour_setup() != 0) {
        exit(1);
        return 1;
    }

    /* The screen is asked about first and the ear taken second: a program that
     * cannot draw has no business holding the keyboard while it says so. */
    TouchTag ear = console_listen();
    if (ear == TOUCH_TAG_INVALID) {
        release_pour();
        println("This machine has no way to hear a key.");
        exit(1);
        return 1;
    }

    uint64_t started_ms = clock_uptime_ms();
    uint64_t frames     = 0;

    int verdict = run_pour(ear, &frames);

    uint64_t ran_ms = clock_uptime_ms() - started_ms;
    say_the_tally(frames, ran_ms, verdict);
    release_pour();

    exit(verdict);
    return verdict;
}
