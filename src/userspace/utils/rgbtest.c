/*
 * rgbtest — the console's colour word is #RRGGBB, end to end.
 *
 * Three oracles, one small utility:
 *
 *   1. Wire round-trip (kernel truth, not the boxlib cache): SET_COLOR with
 *      an arbitrary 24-bit pair, then GET_COLOR read raw off the Hardware
 *      Deck — every bit must come back. Sentinels must resolve to their
 *      role defaults (fg → light gray, bg → black) and nothing else.
 *
 *   2. The printf path: the title and a background-probe line travel
 *      printf → display daemon → VGA ops. The probe line carries a %color
 *      run and two background changes; its space cells land at row 1 with
 *      backgrounds #402060 and #123456 — pixels a host-side screendump
 *      (tools/rgbcheck.sh) compares exactly. One 9-byte colour record
 *      carries the (fg, bg) pair together, so exact background pixels are
 *      exact evidence for the %color foreground riding the same record.
 *
 *   3. The direct path: rows 7-8 are painted with PUTCHAR ops — absolute
 *      coordinates and a colour pair in every op, so no concurrent
 *      writer's cursor can bend the probe. Row 7: CP437 full blocks
 *      (every pixel foreground) in six exact colours. Row 8: spaces
 *      (every pixel background) in two.
 *
 * Prints [RGBTEST] ALL PASS / FAILED; exit code follows.
 */

#include "box/print.h"
#include "box/color.h"
#include "box/vga.h"
#include "box/system.h"
#include "box/core/manifest.h"
#include "box/timeouts.h"

#define HW_VGA_GET_COLOR 0x78

static int g_failures = 0;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("[RGBTEST] FAIL %s\n", what);
        g_failures++;
    }
}

/* Kernel truth, bypassing boxlib's local colour cache. */
static int kernel_pair(Color *fg, Color *bg)
{
    uint32_t out[2] = {0, 0};
    int rc = MfCall1(DECK_HARDWARE, HW_VGA_GET_COLOR,
                     NULL, 0, NULL, 0,
                     out, sizeof(out), NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return rc;
    *fg = out[0];
    *bg = out[1];
    return 0;
}

int main(void)
{
    Color fg = 0, bg = 0;

    /* 1. Wire round-trip — arbitrary 24-bit values, bit-exact. */
    check(vga_setcolor_rgb(COLOR_RGB(0x12, 0x34, 0x56),
                           COLOR_RGB(0x65, 0x43, 0x21)) == 0,
          "set_color(#123456,#654321)");
    check(kernel_pair(&fg, &bg) == 0 &&
          fg == COLOR_RGB(0x12, 0x34, 0x56) &&
          bg == COLOR_RGB(0x65, 0x43, 0x21),
          "get_color returns the exact pair");

    check(vga_setcolor_rgb(COLOR_DEFAULT, COLOR_INHERIT) == 0,
          "set_color(default,inherit)");
    check(kernel_pair(&fg, &bg) == 0 &&
          fg == COLOR_LIGHT_GRAY && bg == COLOR_BLACK,
          "sentinels resolve to light_gray/black");

    check(vga_setcolor_rgb(COLOR_LIGHT_GRAY, COLOR_BLACK) == 0,
          "restore default pair");

    /* 2. The printf path. Direct clear first: black field, cursor (0,0). */
    check(vga_clear_rgb(COLOR_LIGHT_GRAY, COLOR_BLACK) == 0, "clear screen");

    printf("[RGBTEST] the console's colour word is #rrggbb\n");
    /* First background via the colour-state API, second via the %bgcolor
     * specifier — both halves of the pair steerable from a format string. */
    set_color_bg(COLOR_RGB(0x40, 0x20, 0x60));
    printf("%color        ", (Color)COLOR_RGB(0xFE, 0xDC, 0xBA));
    printf("%bgcolor        ", (Color)COLOR_RGB(0x12, 0x34, 0x56));
    set_color_bg(COLOR_BLACK);
    set_color(COLOR_LIGHT_GRAY);
    printf("\n");
    io_flush();

    /* 3. The direct probe — one batched Manifest of PUTCHAR ops. */
    static const Color probe_fg[6] = {
        COLOR_RGB(0xFF, 0x00, 0x00), COLOR_RGB(0x00, 0xFF, 0x00),
        COLOR_RGB(0x00, 0x00, 0xFF), COLOR_RGB(0x10, 0x20, 0x30),
        COLOR_AMBER,                 COLOR_WHITE,
    };
    int rc = 0;
    vga_begin();
    for (int i = 0; i < 6 && rc == 0; i++) {
        for (int c = 0; c < 4 && rc == 0; c++) {
            rc = vga_putchar_at(7, (uint8_t)(i * 4 + c), (char)0xDB,
                                probe_fg[i], COLOR_BLACK);
        }
    }
    for (int c = 0; c < 4 && rc == 0; c++) {
        rc = vga_putchar_at(8, (uint8_t)c, ' ',
                            COLOR_WHITE, COLOR_RGB(0x40, 0x20, 0x60));
    }
    for (int c = 0; c < 4 && rc == 0; c++) {
        rc = vga_putchar_at(8, (uint8_t)(4 + c), ' ',
                            COLOR_WHITE, COLOR_RGB(0x12, 0x34, 0x56));
    }
    if (rc == 0) rc = vga_commit();
    else         (void)vga_commit();
    check(rc == 0, "direct probe painted (PUTCHAR batch)");

    if (g_failures == 0) {
        printf("[RGBTEST] ALL PASS\n");
        exit(0);
        return 0;
    }
    printf("[RGBTEST] FAILED: %d\n", g_failures);
    exit(1);
    return 1;
}
