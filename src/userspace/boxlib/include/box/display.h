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
 * Everything a strand asks the display is a QUESTION (box/ipc.h ask, the
 * kernel's system.ask): it gets exactly one answer — the display's, or, if
 * the display's life ends first, the kernel's -ERR_PROCESS_NOT_FOUND — and
 * who asked is the kernel's word (AskFrom heads the question: the asker's
 * generation and its program), never the asker's. What an answer hands out —
 * a lane, a layer — is taken back when the answer reaches nobody because the
 * asker died first. What needs no answer (LISTEN, a layer's FRAME and CLOSE)
 * is a plain message to the display that granted the strand its lane.
 */

/* The tag the display wears: questions are asked of it. */
#define DISPLAY_TAG       "display"

/* A console lane, asked: [DISP_CMD_LANE]. The lane belongs to the asker's
 * (pid, generation) as the kernel names them. Answer:
 * [DISP_CMD_LANE][u32 display pid]["console:N" NUL] — the display that will
 * read it, and the tag of a Brook it is already reading (the writer arrives
 * at a laid table). [DISP_CMD_LANE] alone is an honest refusal. */
#define DISP_CMD_LANE     0x30

/* Listen, sent to the lane's display: [DISP_CMD_LISTEN][u32 generation]
 * [u8 listening] — with 1 the lane of (sender, generation) hears the keyboard
 * from now on: the daemon says each key again as a Touch on that lane's own
 * tag ("console:N"), payload kb_event_t, and the program edits its line and
 * echoes through the lane.
 * Listening pushes the lane to the top of the daemon's ear stack; 0 gives the
 * ear back, and so does the lane closing — the one beneath hears again, and
 * with nobody listening the keys wait at the daemon for the next reader. No
 * reply: the keys are the answer. */
#define DISP_CMD_LISTEN   0x13
/* Ping: the daemon answers with its pid. Kept for bench's IPC round trip;
 * nothing discovers the daemon by it. */
#define DISP_CMD_PING     0x12
/* Size, asked: [DISP_CMD_SIZE] — the console in cells. Answer:
 * [DISP_CMD_SIZE][u32 cols][u32 rows]. The display holds the screen and its
 * font, so it is the one to ask; the kernel no longer knows cells at all. */
#define DISP_CMD_SIZE     0x14

/* The console in cells, asked of the display (DISP_CMD_SIZE). */
int display_size(uint32_t *cols, uint32_t *rows);

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
/* A hint: text[0..len) drawn in (fg, bg) as TEXT is, but it is something the
 * program shows and never said — a line editor's whisper, the blanks that
 * take it back — so the display keeps it out of the log ring: the serial
 * line and `logsave` hold what was typed, not what was suggested. */
#define CONSOLE_RUN_HINT      0x04

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

/* How far the display has said: the highest order whose text it has said into
 * the log ring, a word of the same page on a cache line of its own
 * (CONSOLE_ORDER_SAID words in), stored the moment an utterance is in the ring
 * and only ever forward. A strand that leaves waits for it to pass its own
 * last frame (print.c, io_depart). A writer that finds the display dead says
 * into the ring itself what is still in its lane past it, and what the display
 * never took (lane_resay) — nothing it pushed is lost to a display that died.
 *
 * Two windows stay, and both are said here rather than left to be found.
 * Killed between saying and storing, the display leaves those lines to be
 * said twice: a few instructions wide, and a line twice is not a line lost.
 * And a frame the display took, numbered below one it has already said — two
 * writers crossing in the merge — is lost if the display dies before saying
 * it: inside one turn of the display, with two writers racing. */
#define CONSOLE_ORDER_SAID    8u

/* --------------------------------------------------------------------------
 * Display layers
 * --------------------------------------------------------------------------
 *
 * ConsoleRun is the text stream; a layer is a picture a program keeps on the
 * screen, composed by the display over the base text by role, then age. The
 * display makes the layer's shared memory for the asking program alone (a Bay
 * made for one program, box/bay.h bay_create_for) and names it in the reply;
 * the program only opens it, and nobody else can. The names are the display's
 * own and reused — display:layer:N — so the volume, which keeps every tag it
 * is ever told, learns as many as were ever shown at once. Three publish
 * slots: the writer fills a FREE one — with none FREE, the oldest READY, a
 * frame nobody will show — and marks it READY; the display takes the newest
 * READY and frees the rest.
 *
 * What a program may hold is bounded by the screen itself: the area of its
 * layers in one role, over all its strands, never exceeds the screen's —
 * there is no more than one screen of picture to show at one depth.
 *
 * Opening a layer and folding one into the text are QUESTIONS that care about
 * the console's order, so they carry it: `order` is the asker's
 * console_order_mark() (box/print.h) — everything it printed is in its lane,
 * and that is the number of its last frame. The display draws no frame
 * numbered past a waiting question's mark until it has answered it, and
 * answers once the asker's own frames up to the mark are drawn: the caret an
 * open names stands after the asker's text, and a fold lands exactly where it
 * was asked — after everything numbered before it, before everything after.
 */

/* Questions: OPEN (DisplayLayerOpenRequest → DisplayLayerOpenReply) and
 * COMMIT (DisplayLayerCommitRequest → DisplayLayerCommitReply). Messages to
 * the display that granted the asker its layer: FRAME and CLOSE. */
#define DISP_CMD_LAYER_OPEN   0x40
#define DISP_CMD_LAYER_FRAME  0x41
#define DISP_CMD_LAYER_CLOSE  0x42
#define DISP_CMD_LAYER_COMMIT 0x43

#define DISPLAY_LAYER_VERSION   4u
#define DISPLAY_LAYER_SLOTS     3u
#define DISPLAY_LAYER_TAG_PREFIX "display:layer:"
#define DISPLAY_LAYER_TAG_MAX   48u

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
    uint8_t  command;         /* DISP_CMD_LAYER_OPEN */
    uint8_t  role;            /* DisplayLayerRole */
    uint16_t flags;           /* none defined: 0 */
    uint32_t reserved;
    uint64_t order;           /* the asker's console_order_mark() */
    int32_t  x;
    int32_t  y;
    uint32_t width;
    uint32_t height;
} DisplayLayerOpenRequest;

typedef struct PACKED {
    uint8_t  command;         /* DISP_CMD_LAYER_OPEN */
    uint8_t  reserved[3];
    int32_t  error;           /* error_t; the rest is meaningful only with 0 */
    uint32_t layer_id;
    uint32_t display_pid;     /* where FRAME and CLOSE go */
    uint32_t cols;            /* the console, in cells */
    uint32_t rows;
    uint32_t cursor_x;        /* where the console's caret stands after the asker's text */
    uint32_t cursor_y;
    char     bay_tag[DISPLAY_LAYER_TAG_MAX];   /* the layer's memory, NUL-terminated */
} DisplayLayerOpenReply;

typedef struct PACKED {
    uint8_t  command;         /* DISP_CMD_LAYER_COMMIT */
    uint8_t  reserved[3];
    uint32_t layer_id;
    uint32_t scroll_up;       /* base rows scrolled away before the fold */
    uint32_t reserved2;
    uint64_t order;           /* the asker's console_order_mark() */
} DisplayLayerCommitRequest;

typedef struct PACKED {
    uint8_t  command;         /* DISP_CMD_LAYER_COMMIT */
    uint8_t  reserved[3];
    int32_t  error;           /* error_t: 0 — the layer is text now, and gone */
} DisplayLayerCommitReply;

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
 * fields. The display writes the geometry before it answers; slot state is
 * changed with compiler atomics by both cabins. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t cell_count;
    uint32_t slot_count;
    uint32_t slot_state[DISPLAY_LAYER_SLOTS];
    uint64_t sequence[DISPLAY_LAYER_SLOTS];
    uint32_t cursor_x[DISPLAY_LAYER_SLOTS];
    uint32_t cursor_y[DISPLAY_LAYER_SLOTS];
    uint32_t cursor_visible[DISPLAY_LAYER_SLOTS];
} DisplayLayerSharedHeader;

typedef struct {
    TextCell Cell;
    uint8_t  Opaque;
    uint8_t  Reserved[3];
} DisplayLayerCell;

#define DISPLAY_LAYER_MAGIC 0x4C594552u /* "LYER" */

STATIC_ASSERT(sizeof(DisplayLayerSharedHeader) == 104,
              "DisplayLayerSharedHeader is a Bay ABI and must stay 104 bytes");
STATIC_ASSERT(sizeof(DisplayLayerOpenRequest) == 32,
              "DisplayLayerOpenRequest is a question's ABI and must stay 32 bytes");
STATIC_ASSERT(sizeof(DisplayLayerOpenReply) == 80,
              "DisplayLayerOpenReply is an answer's ABI and must stay 80 bytes");
STATIC_ASSERT(sizeof(DisplayLayerCommitRequest) == 24,
              "DisplayLayerCommitRequest is a question's ABI and must stay 24 bytes");
STATIC_ASSERT(sizeof(DisplayLayerCommitReply) == 8,
              "DisplayLayerCommitReply is an answer's ABI and must stay 8 bytes");
STATIC_ASSERT(sizeof(DisplayLayerCell) == 16,
              "DisplayLayerCell is a Bay ABI and must stay 16 bytes");
STATIC_ASSERT(sizeof(DisplayLayerCloseRequest) == 12,
              "DisplayLayerCloseRequest is a message ABI and must stay 12 bytes");
STATIC_ASSERT(sizeof(DisplayLayerFrameNotice) == 12,
              "DisplayLayerFrameNotice is a message ABI and must stay 12 bytes");

#ifdef __cplusplus
}
#endif

#endif
