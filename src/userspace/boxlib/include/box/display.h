#ifndef BOX_DISPLAY_H
#define BOX_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "key_chord.h"
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
 * A colour the program did not choose travels as COLOR_DEFAULT (boxos_color.h)
 * and is the display's to draw: as text, the screen's text colour; behind it,
 * the screen's page colour — light gray on black until the keeper of the
 * programs in front says others (DISP_CMD_PAGE). What is on the screen in them
 * is drawn again in the new ones.
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
    uint32_t fg;                          /* #RRGGBB, or COLOR_DEFAULT */
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
 * The program in front, and the ones behind
 * --------------------------------------------------------------------------
 *
 * A hub keeps one program in front of the person — one it started and waits
 * for, or one it brings back — and lets others live behind. The keys it acts
 * on are its own: it says them (DISP_CMD_KEYS), and the display takes them
 * before any program hears them and tells which was pressed, and over what.
 * What each one does is the keeper's; the display only hears. The newest
 * keeper's keys are the ones taken, and with it gone the ones beneath again.
 *
 * Told is the keeper of the program in front, about that program. When that
 * program keeps others itself — a hub started from a hub — and has nothing
 * of its own in front, it is told, about nothing: it is the hub the person
 * looks at. With nothing kept in front, the newest keeper that said keys and
 * is not itself behind, about nothing.
 *
 * A keeper that ends does not leave what it kept behind loose: each stays
 * behind, given to the keeper that kept the one that ended — or, with none,
 * to the next keeper that says its keys — and that keeper is told. What it
 * kept in front is nobody's: shown, and hearing, as before.
 *
 * A program behind hears no key: a strand of it that asks to listen waits,
 * and its keeper is told it wants the keyboard. Nor is its scene or any
 * overlay of it shown — the glass is the program's in front — but its notices
 * are, made to be seen beside whatever is in front. A program it starts,
 * however far down, lives behind with it, and comes forward with it; the
 * strand that asked hears then, and its pictures are shown again.
 *
 * Five QUESTIONS, answered with a DisplayFrontReply — asked only by a
 * program that wears `system` (-ERR_ACCESS_DENIED otherwise), the asker the
 * keeper by the kernel's word. Three are a DisplayFrontRequest:
 *
 *   DISP_CMD_FRONT     the program is in front, kept by the asker
 *   DISP_CMD_BACK      the program lives behind, kept by the asker
 *   DISP_CMD_ASK_END   the program asked to finish: a ConsoleNotice
 *                      CONSOLE_NOTICE_END said on each of its lanes that
 *                      reads now; heard 1 when a strand that reads was told —
 *                      0 says no strand of it will answer
 *
 * and two say what the person chose, which the display keeps no setting of:
 *
 *   DISP_CMD_PAGE      a DisplayPageRequest: the screen's text colour and
 *                      page colour — every colour no program chose
 *                      (COLOR_DEFAULT) drawn in them from now on, what the
 *                      screen shows too; two #RRGGBB, else
 *                      -ERR_INVALID_ARGUMENT
 *   DISP_CMD_KEYS      a DisplayKeysRequest and its KeyChords: the keys this
 *                      keeper acts on, from now on — each held with ctrl or
 *                      alt, else -ERR_INVALID_ARGUMENT; none takes its keys
 *                      back. The first on_screen of them are about the
 *                      program on the screen, and told to whoever keeps it
 *                      even when it is a keeper itself — a hub started from
 *                      a hub is stopped by the one that started it; the rest
 *                      to the keeper whose screen is shown
 *
 * What the display tells a keeper is a Touch on DISPLAY_FRONT_TAG, a
 * DisplayFrontNews, sent by the display (source_pid the reply's display_pid):
 * one of its keys pressed, a program it keeps behind asking for the keyboard,
 * or one behind given to it. And to every keeper at once, when a display comes — at the boot,
 * or raised again after a fall, knowing nothing of what the one before it was
 * told — that it is here: each keeper says again what it keeps, the page it
 * chose and its keys, and learns the display's pid from the answers, never
 * from the news.
 */
#define DISP_CMD_FRONT     0x50
#define DISP_CMD_BACK      0x51
#define DISP_CMD_ASK_END   0x52
#define DISP_CMD_PAGE      0x53
#define DISP_CMD_KEYS      0x54

/* Where programs are, asked of the display by any program: [DISP_CMD_PLACES].
 * Answer: a DisplayPlacesReply, then `count` DisplayPlaces — the program whose
 * screen is shown, each program let go to live behind, each keeper; what the
 * display keeps of who is in front and who behind, and no more (a program
 * started from one behind is behind with it, and not named). */
#define DISP_CMD_PLACES    0x55

enum {
    DISPLAY_PLACE_SHOWN  = 1,   /* its screen is the one shown */
    DISPLAY_PLACE_BEHIND = 2,   /* let go: it lives behind, on its own */
    DISPLAY_PLACE_KEEPER = 4,   /* it keeps others: it said its keys */
};

/* An occurrence the kernel's muster books (touch/muster.h): it stands on any
 * volume, whatever its tags, and never takes a name of a volume's. */
#define DISPLAY_FRONT_TAG  "keeper:news"

typedef enum {
    DISPLAY_FRONT_KEY   = 1,    /* one of its keys: `key` its place among those it said;
                                 * the program 0 when the keeper's own screen is shown */
    DISPLAY_FRONT_WANTS = 2,    /* a program behind asks for the keyboard */
    DISPLAY_FRONT_HERE  = 3,    /* a display came: keeper and program are 0 */
    DISPLAY_FRONT_GIVEN = 4,    /* one behind, kept by a keeper that ended, is this keeper's */
} DisplayFrontWhat;

typedef struct PACKED {
    uint8_t  command;           /* DISP_CMD_FRONT, _BACK or _ASK_END */
    uint8_t  reserved[3];
    uint32_t pid;               /* the program, by its main strand ... */
    uint32_t generation;        /* ... in this incarnation */
} DisplayFrontRequest;

typedef struct PACKED {
    uint8_t  command;           /* DISP_CMD_PAGE */
    uint8_t  reserved[3];
    uint32_t text;              /* #RRGGBB: text no program chose a colour for */
    uint32_t page;              /* #RRGGBB: the screen behind it */
} DisplayPageRequest;

typedef struct PACKED {
    uint8_t command;            /* DISP_CMD_KEYS */
    uint8_t count;              /* the KeyChords that follow, in the order told back */
    uint8_t on_screen;          /* the first of them are about the program on the screen */
    uint8_t reserved;
} DisplayKeysRequest;

typedef struct PACKED {
    uint8_t  command;
    uint8_t  heard;             /* DISP_CMD_ASK_END: a strand of it was told */
    uint8_t  reserved[2];
    int32_t  error;             /* OK, or the display's refusal */
    uint32_t display_pid;       /* who sends the news on DISPLAY_FRONT_TAG */
} DisplayFrontReply;

typedef struct PACKED {
    uint32_t keeper_pid;        /* the program the news is for */
    uint32_t keeper_generation;
    uint8_t  what;              /* DisplayFrontWhat */
    uint8_t  key;               /* DISPLAY_FRONT_KEY: which of the keeper's keys */
    uint8_t  reserved[2];
    uint32_t pid;               /* the program it is about */
    uint32_t generation;
} DisplayFrontNews;

/* The asking program keeps the program (pid, generation) in front of the
 * person, or behind; *display — may be NULL — is the display's pid, the
 * source of its news. OK, or a refusal: the display's, or the ask's. */
int display_front(uint32_t pid, uint32_t generation, uint32_t *display);
int display_back(uint32_t pid, uint32_t generation, uint32_t *display);

/* The program asked to finish: 1 when a strand of it that reads was told, 0
 * when none of its strands reads, or a refusal. */
int display_ask_end(uint32_t pid, uint32_t generation);

/* The screen's text colour and page colour, #RRGGBB, said by the asking
 * keeper; *display — may be NULL — as display_front's. OK, or a refusal. */
int display_page(uint32_t text, uint32_t page, uint32_t *display);

/* The keys the asking keeper acts on, at most 255: told back by their place
 * in `keys` (DISPLAY_FRONT_KEY), the first `on_screen` about the program on
 * the screen (DISP_CMD_KEYS). 0 takes them back. *display as above. */
int display_keys(const KeyChord *keys, uint32_t count, uint32_t on_screen, uint32_t *display);

typedef struct PACKED {
    uint8_t  command;           /* DISP_CMD_PLACES */
    uint8_t  reserved;
    uint16_t count;             /* the DisplayPlaces that follow */
} DisplayPlacesReply;

typedef struct PACKED {
    uint32_t pid;               /* a program, by its main strand */
    uint32_t generation;
    uint8_t  place;             /* DISPLAY_PLACE_*, as many as hold */
    uint8_t  reserved[3];
} DisplayPlace;

/* Where programs are (DISP_CMD_PLACES), into `places`, *count of them: OK;
 * -ERR_BUFFER_TOO_SMALL when there are more than `capacity` (at most 65535);
 * or the ask's refusal. */
int display_places(DisplayPlace *places, uint32_t capacity, uint32_t *count);

/* --------------------------------------------------------------------------
 * An effect over the whole glass
 * --------------------------------------------------------------------------
 *
 * Any program may ask the display to lay an effect over everything it shows —
 * the console's text, every layer, the caret — for a while. The display only
 * changes how the picture is presented: what the programs drew, and what the
 * keeper's page colours are, stay as they are underneath, and are shown
 * as they were the moment the effect ends. Nothing a program reads back from
 * the display, a layer's cells or the console's, is touched.
 *
 * What an effect is: a set of kinds, laid together, with one strength shared
 * by them, which swells in at the start and ebbs out at the end (a mirror has
 * no half-strength and comes and goes at once). Geometry first, then colour,
 * then the sparkles:
 *
 *   DISPLAY_EFFECT_SWAY      rows swing sideways (AXIS_X) and columns up and
 *                            down (AXIS_Y) in slow waves, the cells at the
 *                            edge coming round the other side — the glass
 *                            seen drunk; the stronger, the wider the swing
 *   DISPLAY_EFFECT_MIRROR    the picture turned over: left for right (AXIS_X),
 *                            upside down (AXIS_Y), the glyphs themselves
 *                            as they are drawn
 *   DISPLAY_EFFECT_FADE      the colours drained towards grey
 *   DISPLAY_EFFECT_INVERT    the colours turned to their opposites
 *   DISPLAY_EFFECT_TINT      the colours seen through glass of `color`: each
 *                            of a cell's colours is multiplied by it
 *   DISPLAY_EFFECT_SHIMMER   sparkles come and go on the glass, in `color`
 *                            brightened — on the empty cells as stars, on the
 *                            text as a flash; the stronger, the more of them
 *
 * On a text page, with its sixteen colours and its whole cells, every kind
 * is shown as nearly as that allows: a colour is quantized after the
 * effect changed it, and a swing moves whole cells.
 *
 * Who may ask: any program. An effect is time-limited and changes nothing but
 * the picture, so it asks no badge — and there is one at a time: a new one
 * takes the place of the one laid, whoever laid it, so no program can hold
 * the glass against the next one's asking. How long it lasts is the asker's word,
 * at most DISPLAY_EFFECT_LONGEST_MS: the display ends it itself. It also ends
 * when the program that laid it ends, and when its asker lifts it
 * (DISP_CMD_EFFECT_LIFT) — which a keeper (a program that wears `system`) may
 * do to any effect, the person's way out of one a program laid and forgot.
 *
 * Both are QUESTIONS, answered with a DisplayFrontReply: the display's refusal
 * in `error`; for a lift, `heard` is 1 when an effect was lifted.
 */
#define DISP_CMD_EFFECT       0x56
#define DISP_CMD_EFFECT_LIFT  0x57

enum {
    DISPLAY_EFFECT_TINT    = 1,
    DISPLAY_EFFECT_FADE    = 2,
    DISPLAY_EFFECT_INVERT  = 4,
    DISPLAY_EFFECT_SWAY    = 8,
    DISPLAY_EFFECT_MIRROR  = 16,
    DISPLAY_EFFECT_SHIMMER = 32,
    DISPLAY_EFFECT_KINDS   = 63,    /* every kind there is */
};

enum {
    DISPLAY_EFFECT_AXIS_X = 1,
    DISPLAY_EFFECT_AXIS_Y = 2,
};

#define DISPLAY_EFFECT_LONGEST_MS  300000u

typedef struct PACKED {
    uint8_t  command;           /* DISP_CMD_EFFECT */
    uint8_t  strength;          /* 1..255 */
    uint8_t  axes;              /* DISPLAY_EFFECT_AXIS_*: for SWAY and MIRROR, at least one */
    uint8_t  reserved;
    uint32_t kinds;             /* DISPLAY_EFFECT_*, at least one, none outside DISPLAY_EFFECT_KINDS */
    uint32_t color;             /* #RRGGBB: the tint, and the sparkles' */
    uint32_t duration_ms;       /* 1..DISPLAY_EFFECT_LONGEST_MS */
} DisplayEffectRequest;

/* The asking program lays an effect over the glass (DISP_CMD_EFFECT). OK, or
 * -ERR_INVALID_ARGUMENT for a request outside what is said above, or the ask's
 * refusal. */
int display_effect(uint32_t kinds, uint8_t strength, uint8_t axes, uint32_t color, uint32_t duration_ms);

/* The effect on the glass lifted by the asking program, which laid it, or by
 * a keeper: 1 when one was lifted, 0 when there was none; -ERR_ACCESS_DENIED
 * when another program's lies there and the asker is no keeper. */
int display_effect_lift(void);

/* A word of the display on a lane's own tag, in place of a key — told from a
 * key, which is a kb_event_t of three bytes, by its length and its mark. */
#define CONSOLE_NOTICE_MARK  0x4E4F5445u   /* "ETON", the bytes of NOTE */

typedef enum {
    CONSOLE_NOTICE_END = 1,     /* the program is asked to finish */
} ConsoleNoticeKind;

typedef struct PACKED {
    uint32_t mark;              /* CONSOLE_NOTICE_MARK */
    uint8_t  kind;              /* ConsoleNoticeKind */
    uint8_t  reserved[3];
} ConsoleNotice;

/* The kind of notice `payload` is, 0 when it is none (a key, most often). */
uint8_t console_notice_of(const void *payload, uint32_t length);

/* --------------------------------------------------------------------------
 * Display layers
 * --------------------------------------------------------------------------
 *
 * ConsoleRun is the text stream; a layer is a picture a program keeps on the
 * screen, composed by the display over the base text by role, then age. The
 * display makes the layer's shared memory for the asking program alone (a Bay
 * made for one program, box/bay.h bay_create_for) and names it in the reply;
 * the program only opens it, and nobody else can. A cell may carry a mark the
 * display draws over its glyph in a colour of its own — a line under it, a
 * bar down its left side — so a caret a program draws stands on a letter and
 * covers nothing; folded into the text, a cell keeps its glyph and colours,
 * not its mark. The names are the display's
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

#define DISPLAY_LAYER_VERSION   5u
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

/* What a cell's mark is: drawn in the cell's MarkColor over its glyph, on a
 * framebuffer as pixels; the text page has none, and shows a marked cell in
 * the mark's colour behind its glyph instead. */
typedef enum {
    DISPLAY_CELL_MARK_NONE  = 0,
    DISPLAY_CELL_MARK_UNDER = 1,   /* a line under the glyph: the cell's two lowest pixel rows */
    DISPLAY_CELL_MARK_BAR   = 2,   /* a bar down its left side: the cell's two leftmost pixel columns */
} DisplayCellMark;

typedef struct {
    TextCell Cell;            /* fg and bg: #RRGGBB, or COLOR_DEFAULT */
    uint32_t MarkColor;       /* #RRGGBB, or COLOR_DEFAULT: the screen's text colour */
    uint8_t  Opaque;
    uint8_t  Mark;            /* DisplayCellMark; another is none */
    uint8_t  Reserved[2];
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
STATIC_ASSERT(sizeof(DisplayLayerCell) == 20,
              "DisplayLayerCell is a Bay ABI and must stay 20 bytes");
STATIC_ASSERT(sizeof(DisplayLayerCloseRequest) == 12,
              "DisplayLayerCloseRequest is a message ABI and must stay 12 bytes");
STATIC_ASSERT(sizeof(DisplayLayerFrameNotice) == 12,
              "DisplayLayerFrameNotice is a message ABI and must stay 12 bytes");
STATIC_ASSERT(sizeof(DisplayFrontRequest) == 12 && sizeof(DisplayPageRequest) == 12 &&
                  sizeof(DisplayFrontReply) == 12 && sizeof(DisplayKeysRequest) == 4 && sizeof(KeyChord) == 4 &&
                  sizeof(DisplayPlacesReply) == 4 && sizeof(DisplayPlace) == 12 &&
                  sizeof(DisplayEffectRequest) == 16,
              "the front's questions and answers are an ABI of fixed sizes");
STATIC_ASSERT(sizeof(DisplayFrontNews) == 20, "DisplayFrontNews is a Touch payload ABI of 20 bytes");
STATIC_ASSERT(sizeof(ConsoleNotice) == 8, "ConsoleNotice is a Touch payload ABI of 8 bytes, never a key's 3");

#ifdef __cplusplus
}
#endif

#endif
