#ifndef BOXOS_SCREEN_H
#define BOXOS_SCREEN_H

/*
 * boxos_screen.h — the Screen, shared kernel/userspace.
 *
 * The screen is a resource of one system program — the display — and the
 * kernel only lends it. `hw.screen.claim` maps the glass into the claimer's
 * cabin at CABIN_SCREEN_BASE and answers this description; `hw.screen.release`
 * or the end of the cabin gives it back, and the kernel's Emergency Screen
 * shows the tail of the log ring until somebody claims it again.
 *
 * Kernel:    #include "ktypes.h" first.
 * Userspace: #include "box/types.h" first.
 */

#define SCREEN_KIND_TEXT    1u   /* 80x25 legacy VGA text: two bytes a cell, glyph then attribute */
#define SCREEN_KIND_PIXELS  2u   /* linear framebuffer, four bytes a pixel */

typedef struct ScreenDescription {
    uint32_t kind;      /* SCREEN_KIND_*                                          */
    uint32_t width;     /* PIXELS: pixels; TEXT: cells                            */
    uint32_t height;    /* PIXELS: pixels; TEXT: cells                            */
    uint32_t stride;    /* bytes from the start of one row to the next            */
    uint32_t format;    /* PIXELS: BOX_PIXEL_* (boxos_pixel.h); TEXT: 0           */
    uint32_t reserved;
    uint64_t va;        /* the first byte of the glass in the claimer's cabin     */
} ScreenDescription;

#ifdef __cplusplus
static_assert(sizeof(ScreenDescription) == 32,
              "ScreenDescription is a kernel/userspace ABI and must stay 32 bytes");
#else
_Static_assert(sizeof(ScreenDescription) == 32,
               "ScreenDescription is a kernel/userspace ABI and must stay 32 bytes");
#endif

/*
 * The Touch tag the kernel publishes the screen's turns on. Payload: one
 * uint32_t, a SCREEN_EVENT_*.
 *
 *   AWAY — Ctrl+Alt+Esc showed the Emergency Screen. The holder keeps its
 *          window, which now leads to one page of ordinary memory: drawing
 *          goes nowhere and nothing faults.
 *   BACK — the second Ctrl+Alt+Esc gave the glass back. What is on it is the
 *          Emergency Screen's, so the holder draws its whole picture again.
 *   FREE — nobody holds the screen any more; a claim refused with ERR_BUSY
 *          (the previous holder's cabin not yet torn down) waits for this.
 */
#define SCREEN_TOUCH_TAG    "screen"
#define SCREEN_EVENT_AWAY   1u
#define SCREEN_EVENT_BACK   2u
#define SCREEN_EVENT_FREE   3u

#endif /* BOXOS_SCREEN_H */
