
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

#define FRAME_US       40000u

#define HUE_TURN       1536u
#define ROW_STEP       48u
#define ROW_DOWN       (HUE_TURN - ROW_STEP)

#define PHASE_FRAC     256u
#define PHASE_TURN     (HUE_TURN * PHASE_FRAC)

#define SPEED_SLOW_HZ  300u
#define SPEED_FAST_HZ  900u
#define DRIFT_HZ       40u

static uint64_t g_rng;
static uint32_t g_drift;

static vga_dimensions_t g_dim;
static TextCell *g_frame;

static uint32_t *s_phase;
static uint32_t *s_speed;

static uint64_t rng_next(void)
{
    uint64_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng = x;
    return x;
}

#define PHASE_RAMP     12u
#define PHASE_WALK     (6u * PHASE_FRAC)
#define SPEED_WALK     (24u * PHASE_FRAC)

static uint32_t walk_step(uint32_t span)
{
    return (uint32_t)(rng_next() % (2u * span + 1u)) - span;
}

static void seed_columns(unsigned cols)
{
    g_rng = cpu_rdtsc() | 1u;

    uint32_t phase = (uint32_t)(rng_next() % PHASE_TURN);
    uint32_t speed = (SPEED_SLOW_HZ + SPEED_FAST_HZ) / 2u * PHASE_FRAC;

    const uint32_t slow = SPEED_SLOW_HZ * PHASE_FRAC;
    const uint32_t fast = SPEED_FAST_HZ * PHASE_FRAC;

    for (unsigned c = 0; c < cols; c++) {
        s_phase[c] = phase;
        s_speed[c] = speed;

        phase = (phase + PHASE_RAMP * PHASE_FRAC + walk_step(PHASE_WALK)) % PHASE_TURN;

        uint32_t next = speed + walk_step(SPEED_WALK);
        if (next < slow || next > fast) next = speed - (next - speed);
        speed = (next < slow || next > fast) ? speed : next;
    }
}

static void advance(unsigned cols, uint64_t dt_us)
{
    for (unsigned c = 0; c < cols; c++) {
        uint32_t step = (uint32_t)(((uint64_t)s_speed[c] * dt_us) / 1000000u);
        s_phase[c] = (s_phase[c] + step) % PHASE_TURN;
    }

    uint32_t drift_step = (uint32_t)(((uint64_t)DRIFT_HZ * PHASE_FRAC * dt_us) / 1000000u);
    g_drift = (g_drift + drift_step) % PHASE_TURN;
}

static void compose(TextCell *frame, unsigned rows, unsigned cols)
{
    for (unsigned r = 0; r < rows; r++) {
        uint32_t row_hue = ((uint32_t)r * ROW_DOWN) % HUE_TURN;

        TextCell *line = &frame[(size_t)r * cols];

        for (unsigned c = 0; c < cols; c++) {
            uint32_t hue = ((s_phase[c] + g_drift) / PHASE_FRAC + row_hue) % HUE_TURN;

            Color k = color_wheel((uint16_t)hue, 255, 255);
            line[c].fg = k;
            line[c].bg = k;
        }
    }
}

static void park_caret(void)
{
    vga_setcursor((uint8_t)(g_dim.rows - 1), (uint8_t)(g_dim.cols - 1));
}

static void release_pour(void)
{
    free(g_frame); g_frame = NULL;
    free(s_phase); s_phase = NULL;
    free(s_speed); s_speed = NULL;
}

static int pour_setup(void)
{
    if (vga_getdimensions(&g_dim) != 0) {
        println("The screen will not say how big it is.");
        return 1;
    }

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

static int run_pour(TouchTag ear, uint64_t *frames)
{
    uint64_t last_us = clock_uptime_us();
    uint64_t next_us = last_us;

    for (;;) {
        uint64_t now = clock_uptime_us();

        if (now >= next_us) {
            uint64_t dt_us = now - last_us;
            last_us = now;

            advance(g_dim.cols, dt_us);
            compose(g_frame, g_dim.rows, g_dim.cols);

            park_caret();
            if (vga_paint(0, 0, g_dim.rows, g_dim.cols, g_frame) != 0) return 1;
            (*frames)++;

            next_us = clock_uptime_us() + FRAME_US;
            continue;
        }

        uint32_t left_ms = (uint32_t)((next_us - now) / 1000u);
        if (left_ms == 0) left_ms = 1;

        Touch t;
        int rc = touch_await(ear, &t, left_ms);
        if (rc == 0) {
            if (t.payload_len >= sizeof(kb_event_t)) return 0;
        } else if (rc != -ERR_TIMEOUT) {
            return 0;
        }
    }
}

static void say_the_tally(uint64_t frames, uint64_t ran_ms, int code)
{
    console_unlisten();
    vga_clear_rgb(COLOR_LIGHT_GRAY, COLOR_BLACK);
    vga_setcursor(0, 0);

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