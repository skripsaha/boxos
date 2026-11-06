#ifndef BOX_DISPLAY_H
#define BOX_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "text_cell.h"

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
 * `order` is the number this frame took from the console's ONE order, the
 * instant before it was pushed. The daemon renders the smallest pending
 * number first, so causally-ordered output — a child's dying words, then the
 * death, then the shell's next prompt — renders in cause order no matter
 * which lane each frame rode or when the daemon got scheduled.
 *
 * ‼ THIS FIELD USED TO BE THE WRITER'S rdtsc, AND THAT WAS THE DEFECT. The
 * header claimed "TSC is AMP-synced on every supported machine". It is not:
 * per_core.c tolerates four microseconds at boot sync and the cores drift
 * after that, while a program listing a volume pushes its whole output in
 * less time than that. MEASURED on the board — four listings of twelve cut
 * open at byte 891 of 4235, the same byte every time, which is a fixed offset
 * between two cores and not a race. The prompt of the next command was being
 * rendered INSIDE the output of the one that had just finished. A counter has
 * no tolerance and nothing to drift; see CONSOLE_ORDER_TAG below. */
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
    uint64_t order;                       /* the console's shared order — see
                                           * console_order_next in print.c */
    char     text[CONSOLE_RUN_TEXT_MAX];
} ConsoleRun;

STATIC_ASSERT(sizeof(ConsoleRun) == 128,
              "ConsoleRun is the console-lane frame ABI — must stay 128 bytes");

/* Ring depth of one lane: 256 frames = 32 KiB of slots + one header page.
 * Physics of the ring, not a bound on the stream — a writer that outruns
 * the daemon by more than this simply waits its turn. */
#define CONSOLE_LANE_FRAMES   256u

/* Where the console's one order lives: a single counter in memory every cabin
 * that prints can see. Not a lane number — lanes are "console:0", "console:1"
 * and so on, and this is the one name in that family that is not a number.
 *
 * The display daemon CREATES it before it grants its first lane and holds it
 * for as long as it lives (a Bay is refcounted, and a counter that went away
 * with its last writer would start again at zero, behind everything still
 * banked in the lanes). A writer maps it as part of taking its lane: having a
 * lane and not having the order is not a state this console can be in, because
 * a frame stamped from any other source is a different DOMAIN of numbers and
 * two domains in one sorted merge order nothing at all.
 *
 * One page, because the counter shares its cache line with nothing: sixteen
 * strands doing a locked add on it is one line moving between cores, and not a
 * line that also carries something somebody else is reading. */
#define CONSOLE_ORDER_TAG     "console:order"
#define CONSOLE_ORDER_BYTES   4096u

/* --------------------------------------------------------------------------
 * Display layers
 * --------------------------------------------------------------------------
 *
 * ConsoleRun remains the text stream contract.  Layers are a separate
 * userspace display-server contract: a client owns a shared Bay containing
 * three publish slots, while display.elf owns composition and the final
 * vga_paint.  The kernel never learns about z-order or layer lifetime.
 */

#define DISP_CMD_LAYER_OPEN   0x40
#define DISP_CMD_LAYER_FRAME  0x41
#define DISP_CMD_LAYER_CLOSE  0x42

#define DISPLAY_LAYER_VERSION 1u
#define DISPLAY_LAYER_SLOTS   3u
#define DISPLAY_LAYER_TAG_MAX 64u

typedef enum {
    DISPLAY_LAYER_SCENE   = 1,
    DISPLAY_LAYER_OVERLAY = 2,
    DISPLAY_LAYER_NOTICE  = 3,
} DisplayLayerRole;

typedef enum {
    DISPLAY_LAYER_FREE    = 0,
    DISPLAY_LAYER_WRITING = 1,
    DISPLAY_LAYER_READY   = 2,
    DISPLAY_LAYER_READING = 3,
} DisplayLayerSlotState;

typedef struct PACKED {
    uint8_t  command;
    uint8_t  role;
    uint16_t flags;
    uint32_t generation;
    int16_t  x;
    int16_t  y;
    uint16_t width;
    uint16_t height;
    char     bay_tag[DISPLAY_LAYER_TAG_MAX];
} DisplayLayerOpenRequest;

typedef struct PACKED {
    uint8_t  command;
    uint8_t  reserved[3];
    int32_t  error;
    uint32_t layer_id;
    uint32_t display_pid;
} DisplayLayerOpenReply;

typedef struct PACKED {
    uint8_t  command;
    uint8_t  reserved[3];
    uint32_t layer_id;
    uint32_t generation;
} DisplayLayerCloseRequest;

typedef struct PACKED {
    uint8_t  command;
    uint8_t  reserved[3];
    uint32_t layer_id;
    uint32_t generation;
} DisplayLayerFrameNotice;

/* The shared Bay header is deliberately fixed-size and contains only POD
 * fields.  Slot state is changed with compiler atomics by both cabins. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t cell_count;
    uint32_t slot_count;
    uint32_t slot_state[DISPLAY_LAYER_SLOTS];
    uint64_t sequence[DISPLAY_LAYER_SLOTS];
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t cursor_visible;
} DisplayLayerSharedHeader;

typedef struct {
    TextCell Cell;
    uint8_t  Opaque;
    uint8_t  Reserved[3];
} DisplayLayerCell;

#define DISPLAY_LAYER_MAGIC 0x4C594552u /* "LYER" */

#ifdef __cplusplus
static_assert(sizeof(DisplayLayerSharedHeader) == 80,
              "DisplayLayerSharedHeader is a Bay ABI and must stay 80 bytes");
static_assert(sizeof(DisplayLayerOpenRequest) == 80,
              "DisplayLayerOpenRequest is an IPC ABI and must stay 80 bytes");
static_assert(sizeof(DisplayLayerOpenReply) == 16,
              "DisplayLayerOpenReply is an IPC ABI and must stay 16 bytes");
static_assert(sizeof(DisplayLayerCell) == 16,
              "DisplayLayerCell is a Bay ABI and must stay 16 bytes");
static_assert(sizeof(DisplayLayerCloseRequest) == 12,
              "DisplayLayerCloseRequest is an IPC ABI and must stay 12 bytes");
static_assert(sizeof(DisplayLayerFrameNotice) == 12,
              "DisplayLayerFrameNotice is an IPC ABI and must stay 12 bytes");
#else
_Static_assert(sizeof(DisplayLayerSharedHeader) == 80,
               "DisplayLayerSharedHeader is a Bay ABI and must stay 80 bytes");
_Static_assert(sizeof(DisplayLayerOpenRequest) == 80,
               "DisplayLayerOpenRequest is an IPC ABI and must stay 80 bytes");
_Static_assert(sizeof(DisplayLayerOpenReply) == 16,
               "DisplayLayerOpenReply is an IPC ABI and must stay 16 bytes");
_Static_assert(sizeof(DisplayLayerCell) == 16,
               "DisplayLayerCell is a Bay ABI and must stay 16 bytes");
_Static_assert(sizeof(DisplayLayerCloseRequest) == 12,
               "DisplayLayerCloseRequest is an IPC ABI and must stay 12 bytes");
_Static_assert(sizeof(DisplayLayerFrameNotice) == 12,
               "DisplayLayerFrameNotice is an IPC ABI and must stay 12 bytes");
#endif

#ifdef __cplusplus
}
#endif

#endif
