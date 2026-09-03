#ifndef BOX_DISPLAY_H
#define BOX_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"

/*
 * Console-stream protocol between a printing strand and the display daemon.
 *
 * Output rides per-strand Brook lanes ("console:N"): the strand asks for a
 * lane once (DISP_CMD_LANE), the daemon opens the reader side FIRST and
 * answers with the lane's tag, and from then on every printed run travels
 * as a ConsoleRun frame — colour is frame METADATA, a full pair of #RRGGBB
 * values, never an escape sequence inside the text. A full ring blocks the
 * writer (brook_push) instead of dropping the batch.
 *
 * The ResultRing wire keeps only the request/reply commands below.
 */

/* Request a console lane. Reply: [DISP_CMD_LANE]["console:N" NUL] — the tag
 * of a Brook the daemon is already reading (writer arrives at a laid table).
 * A 1-byte reply (command echo alone) is an honest refusal. */
#define DISP_CMD_LANE     0x30

#define DISP_CMD_READLINE 0x10
#define DISP_CMD_GETCHAR  0x11
#define DISP_CMD_PING     0x12

/* ConsoleRun — one frame of a console lane. 128 bytes: 20-byte header +
 * up to 108 text bytes. `len` counts valid text; '\n' travels inside the
 * text (it is content, not a control record). The (fg, bg) pair applies to
 * the whole run; a colour change starts a new frame.
 *
 * `tsc` is the writer's rdtsc at push time. The daemon renders the
 * globally-oldest pending frame first (a min-TSC merge across lanes), so
 * causally-ordered output — a child's dying words, then the death, then
 * the shell's next prompt — renders in cause order no matter which lane
 * each frame rode or when the daemon got scheduled. TSC is AMP-synced on
 * every supported machine; micro-inversions between causally-unrelated
 * writers are the same "no cross-writer promise" the console always had. */
#define CONSOLE_RUN_TEXT      0x01  /* render text[0..len) in (fg, bg) */
#define CONSOLE_RUN_CLEAR     0x02  /* clear the screen to (fg, bg) */

#define CONSOLE_RUN_TEXT_MAX  108u

typedef struct PACKED {
    uint8_t  kind;                        /* CONSOLE_RUN_* */
    uint8_t  len;                         /* valid bytes in text[] */
    uint16_t _reserved;
    uint32_t fg;                          /* #RRGGBB, resolved — no sentinels */
    uint32_t bg;
    uint64_t tsc;                         /* push-time rdtsc — global order */
    char     text[CONSOLE_RUN_TEXT_MAX];
} ConsoleRun;

STATIC_ASSERT(sizeof(ConsoleRun) == 128,
              "ConsoleRun is the console-lane frame ABI — must stay 128 bytes");

/* Ring depth of one lane: 256 frames = 32 KiB of slots + one header page.
 * Physics of the ring, not a bound on the stream — a writer that outruns
 * the daemon by more than this simply waits its turn. */
#define CONSOLE_LANE_FRAMES   256u

#ifdef __cplusplus
}
#endif

#endif
