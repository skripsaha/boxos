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

/* Request a console lane: [DISP_CMD_LANE][u32 generation] — the asker's own
 * generation (strand_self_generation), because a pid is reused and a lane
 * must belong to (pid, generation), never to a number. Reply:
 * [DISP_CMD_LANE]["console:N" NUL] — the tag of a Brook the daemon is already
 * reading (writer arrives at a laid table). A 1-byte reply (command echo
 * alone) is an honest refusal. */
#define DISP_CMD_LANE     0x30

/* Listen: [DISP_CMD_LISTEN][u32 generation][u8 listening] — with 1 the lane
 * of (sender, generation) hears the keyboard from now on: the daemon says
 * each key again as a Touch on that lane's own tag ("console:N"), payload
 * kb_event_t, and the program edits its line and echoes through the lane.
 * Listening pushes the lane to the top of the daemon's ear stack; 0 gives the
 * ear back, and so does the lane closing — the one beneath hears again, and
 * with nobody listening the keys wait at the daemon for the next reader. No
 * reply: the keys are the answer. */
#define DISP_CMD_LISTEN   0x13
/* Ping: the daemon answers with its pid. Kept for bench's IPC round trip;
 * nothing discovers the daemon by it. */
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
#define CONSOLE_RUN_STEP      0x03  /* move the cursor: text[0..4) = int32 cells, len 4 */

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
