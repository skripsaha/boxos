#ifndef BOX_ITEMS_H
#define BOX_ITEMS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/color.h"
#include "box/types.h"

/*
 * Items — what a program shows, described to the display: the language of
 * the glass (the owner's design of 2026-11-17, parts 1 to 16).
 *
 * A program does not draw: it says what it shows, and the display keeps it
 * and draws it. What it shows is ITEMS — a text, a shape, a picture, a canvas
 * of its own pixels, cells, or a group of others — each named by a number the
 * program chooses, each with a role (a heading, a button, the page) and
 * properties, every one of them optional: what a program does not say is the
 * role's, as the theme draws that role, and what the theme does not say is
 * the system's. The program changes only what changed — "item 3: its text is
 * 12:46" — and the display draws again only where that changed.
 *
 * WHERE. A program is given an AREA by the display (DISP_CMD_AREA_OPEN): over
 * everything, the whole screen, or in the flow of the console. Where it lies
 * and how large it is are the display's to choose, and it says them — at the
 * opening and whenever they change (ITEM_EVENT_AREA); a program never decides
 * them. An item lies in its parent group, or in the area itself, its place
 * counted from the group's corner.
 *
 * UNITS. Places and sizes are POINTS to 1/256 (ITEM_POINT). The display lays
 * a point on the glass at its scale — one pixel to a point on a screen 1080
 * pixels tall, two on one 2160 tall, as the person sets it — so a program
 * written once looks the same on every screen. A turn is 65536 (ITEM_TURN),
 * a scale of one 65536 (ITEM_ONE), whole opacity 65535 (ITEM_OPAQUE).
 *
 * HOW. The area's changes travel in a ring of shared memory the display makes
 * for the program alone (a Bay, box/bay.h bay_create_for): records one after
 * another, each a whole number of eight-byte words, none crossing the ring's
 * end. Changes go in STEPS: a step's records end with ITEM_OP_STEP, the
 * program sends one message (DISP_CMD_AREA_STEP) — no call for each change —
 * and the display shows a step whole or not yet, never half of one. Pictures,
 * canvases and fonts — anything large — lie in BLOCKS of their own, made by
 * the display for the program (DISP_CMD_BLOCK_OPEN) and named by number from
 * as many items as show them; nothing large is copied into the ring.
 *
 * Should the display fall behind and the ring fill, the program never waits:
 * what it changes meanwhile it keeps, a property's last word over the words
 * before it, and puts in the ring as room comes (painter/gui/area.h). Nothing
 * is lost but what was said over.
 *
 * IN TIME. A change may take time: "item 7: opacity to whole in 200 ms,
 * slowing at the end" (ITEM_OP_ANIMATE) — and the display plays it by the
 * clock, frame by frame, while the program sleeps. It goes from where the
 * property is when it begins to what was said, along a curve, after a delay
 * or after another animation ends, once or again and again; a property of
 * numbers or colours goes through the values between (colours as the eye
 * sees them, through OKLab), one that has none between — characters, a
 * picture, a role, a look of another make — fades from what it was into what
 * it is, and hidden fades out or in. What is said meanwhile of that property
 * cuts it where it stands: said plainly, it is at once what was said; said as
 * another animation, it goes on from there, a spring keeping its speed. An
 * item never shown takes at once a property it never said — there is nothing
 * to go from — and one said plainly before an animation of it, in its step,
 * is where it starts. Where
 * the person asks for less motion, things moved, sized, turned or reshaped
 * are at once where they go and fades are short; and every animation plays
 * at the person's speed. On a text page every change is at once what it
 * says. A frame is drawn only while something seen moves.
 *
 * WHAT COMES BACK rides a second ring in the same memory, the display's
 * events: the area's size and scale, an item's size where it is sized by its
 * content, a change refused and why, a step shown and what showing it cost,
 * an animation ended. The display touches the area's tag (its Bay's name)
 * when it adds one.
 *
 * BOUNDS. Everything a program says is held to the bounds below, per program
 * over all its areas; past one, the change is refused — told back, never a
 * fall — and a ring that breaks the language is closed, the program told.
 *
 * A record the display does not know — a property of a later language — is
 * passed over, said once: what is added never breaks what was.
 */

/* ---- Units ------------------------------------------------------------- */

#define ITEM_POINT        256          /* a point: places and sizes */
#define ITEM_TURN         65536        /* a whole turn, clockwise */
#define ITEM_ONE          65536        /* a scale of one; a line height of one */
#define ITEM_OPAQUE       65535        /* whole opacity */
#define ITEM_BY_CONTENT   ((int32_t)0x80000000u)   /* a size the item's content decides */
#define ITEM_TOP          0xFFFFFFFFu  /* above every sibling */
#define ITEM_AREA         0u           /* the parent of an item in the area itself */

/* ---- What an item is --------------------------------------------------- */

enum {
    ITEM_GROUP   = 1,   /* holds others: their place, clip, turn, scale and opacity together */
    ITEM_TEXT    = 2,   /* text laid out in its box (Typecase) */
    ITEM_SHAPE   = 3,   /* a rectangle, rounded or not, an ellipse, or any outline */
    ITEM_PICTURE = 4,   /* a picture block shown at its size */
    ITEM_CANVAS  = 5,   /* the program's own pixels: a canvas block of two pages */
    ITEM_CELLS   = 6,   /* characters in cells, the console's look: the console itself, a program of cells */
    ITEM_KINDS   = 7,
};

/* What an item means: the theme draws a role, and the person changes how a
 * role looks once for every program. */
enum {
    ITEM_ROLE_NONE      = 0,
    ITEM_ROLE_PAGE      = 1,    /* what everything else stands on */
    ITEM_ROLE_PANEL     = 2,    /* a plate raised from the page */
    ITEM_ROLE_BODY      = 3,    /* running text */
    ITEM_ROLE_HEADING   = 4,
    ITEM_ROLE_CODE      = 5,
    ITEM_ROLE_HINT      = 6,    /* text said quietly */
    ITEM_ROLE_LABEL     = 7,
    ITEM_ROLE_BUTTON    = 8,
    ITEM_ROLE_FIELD     = 9,    /* where text is typed */
    ITEM_ROLE_ICON      = 10,
    ITEM_ROLE_SEPARATOR = 11,
    ITEM_ROLE_SELECTION = 12,
    ITEM_ROLES          = 13,
};

/* ---- Records ----------------------------------------------------------- */

/* Every record begins so: what it does, how long it is in eight-byte words
 * with this head, and the item it is about. */
typedef struct {
    uint16_t Op;
    uint16_t Words;
    uint32_t Item;
} ItemRecord;

enum {
    ITEM_OP_PAD     = 0,   /* nothing: Words of them — or, of no words, the rest of the ring up to its end */
    ITEM_OP_CREATE  = 1,   /* ItemCreate: the item made, in its parent above a sibling */
    ITEM_OP_REMOVE  = 2,   /* the item gone, and everything in it */
    ITEM_OP_RESTACK = 3,   /* ItemPlaceAmong: the item moved to another parent, or above another sibling */
    ITEM_OP_CHANGE  = 4,   /* ItemChange and a value: one property said */
    ITEM_OP_RESET   = 5,   /* ItemChange alone: one property unsaid — the role's again */
    ITEM_OP_BLOCK   = 6,   /* ItemBlockChanged: a block's pixels changed (Item is the block) */
    ITEM_OP_STEP    = 7,   /* ItemStep: the step whole — show it */
    ITEM_OP_CELLS   = 8,   /* ItemCellsRun and its cells: a cells item's cells written along a row */
    ITEM_OP_ANIMATE = 9,   /* ItemAnimation, then ItemChange and a value: one property said, reached in time */
};

typedef struct {
    uint8_t  Kind;        /* ITEM_GROUP ... */
    uint8_t  Reserved;
    uint16_t Role;        /* ITEM_ROLE_* */
    uint32_t Parent;      /* a group's item, or ITEM_AREA */
    uint32_t Above;       /* the sibling it lies above: 0 beneath them all, ITEM_TOP above them all */
    uint32_t Reserved2;
} ItemCreate;

typedef struct {
    uint32_t Parent;
    uint32_t Above;
} ItemPlaceAmong;

typedef struct {
    uint16_t Property;    /* ITEM_PROP_* */
    uint16_t Reserved;
    uint32_t Bytes;       /* the value's bytes that follow */
} ItemChange;

typedef struct {
    int32_t  X, Y, Width, Height;   /* the pixels that changed, in the block's own */
    uint32_t Page;                  /* a canvas: the page they are in, now to be shown */
    uint32_t Reserved;
} ItemBlockChanged;

typedef struct {
    uint64_t Number;      /* the program's count of its steps */
} ItemStep;

/* How a property goes to what was said: along a curve of progress over the
 * time it takes — slowly at first (IN), slowing at the end (OUT), both, in
 * Steps jumps, as a spring that may bounce past its end and settles in about
 * the time it takes, or along the program's own curve. */
enum {
    ITEM_CURVE_LINEAR = 0,
    ITEM_CURVE_IN     = 1,
    ITEM_CURVE_OUT    = 2,
    ITEM_CURVE_IN_OUT = 3,
    ITEM_CURVE_STEPS  = 4,   /* Steps jumps, each at the end of its share of the time */
    ITEM_CURVE_SPRING = 5,   /* Bend[0]: how far past its end it bounces, a share of the way of ITEM_ONE; 0 none */
    ITEM_CURVE_OWN    = 6,   /* Bend: a cubic Bézier's inner points x1, y1, x2, y2 of ITEM_ONE, x within 0 to ITEM_ONE */
    ITEM_CURVES       = 7,
};
#define ITEM_FOREVER 0xFFFFFFFFu   /* repeats that never end, until it is cut */

/* An animation: Number, the program's name for it — the end of the last one
 * of that Number told back (ITEM_EVENT_ANIMATION_END), another waiting on it
 * (After); 0 names none. It begins Delay after it is shown — or after the
 * animations of Number After all end, finished or cut, if any are under way
 * or waiting when its step is whole. Repeats: times again after the first;
 * Back: each one after the first goes back the way the one before it came,
 * and it always ends where a run forward ends — at what was said. */
typedef struct {
    uint32_t Number;
    uint32_t After;
    uint32_t Delay;       /* milliseconds */
    uint32_t Duration;    /* milliseconds a run takes */
    uint8_t  Curve;       /* ITEM_CURVE_* */
    uint8_t  Steps;       /* ITEM_CURVE_STEPS: how many, at least one */
    uint8_t  Back;
    uint8_t  Reserved;
    uint32_t Repeats;     /* or ITEM_FOREVER */
    int32_t  Bend[4];
} ItemAnimation;

/* ---- Properties and their values ---------------------------------------- */

enum {
    ITEM_PROP_PLACE       = 1,    /* ItemPoint: its corner, in its parent */
    ITEM_PROP_SIZE        = 2,    /* ItemPoint: width and height, either ITEM_BY_CONTENT */
    ITEM_PROP_OPACITY     = 3,    /* ItemOpacity */
    ITEM_PROP_HIDDEN      = 4,    /* ItemFlag: not shown */
    ITEM_PROP_TURN        = 5,    /* ItemTurn: about its anchor */
    ITEM_PROP_SCALE       = 6,    /* ItemPoint, of ITEM_ONE: about its anchor */
    ITEM_PROP_ANCHOR      = 7,    /* ItemPoint: where it turns and scales about, from its corner */
    ITEM_PROP_CLIP        = 8,    /* ItemFlag: a group's items seen only within it, its corners too */
    ITEM_PROP_CORNERS     = 9,    /* ItemCorners: each corner rounded */
    ITEM_PROP_SHAPE       = 10,   /* ItemShape: a shape's outline */
    ITEM_PROP_FILLS       = 11,   /* ItemFills: what it is filled with, one over another */
    ITEM_PROP_STROKE      = 12,   /* ItemStroke: a line along its outline */
    ITEM_PROP_SHADOWS     = 13,   /* ItemShadows: raised, pressed */
    ITEM_PROP_TEXT        = 14,   /* ItemBytes: a text's characters, UTF-8 */
    ITEM_PROP_TEXT_STYLE  = 15,   /* ItemTextStyle: how all of it is set */
    ITEM_PROP_TEXT_SPANS  = 16,   /* ItemSpans: stretches of it set otherwise */
    ITEM_PROP_TEXT_BOX    = 17,   /* ItemTextBox: how it is laid in its size */
    ITEM_PROP_SECRET      = 18,   /* ItemFlag: shown as dots, never copied, never read aloud */
    ITEM_PROP_CAPTION     = 19,   /* ItemBytes: what it is, said in words, where it cannot be seen */
    ITEM_PROP_PICTURE     = 20,   /* ItemPictureOf: a picture item's block and how it fills its size */
    ITEM_PROP_CANVAS      = 21,   /* ItemBlockOf: a canvas item's block */
    ITEM_PROP_ROLE        = 22,   /* ItemRoleOf */
    ITEM_PROP_GRID        = 23,   /* ItemGrid: a cells item's columns and rows */
    ITEM_PROP_CARET       = 24,   /* ItemCaret: where a cells item's caret stands */
    ITEM_PROPS            = 25,
};

typedef struct { int32_t X, Y; } ItemPoint;
typedef struct { uint16_t Opacity; uint16_t Reserved; } ItemOpacity;
typedef struct { uint8_t On; uint8_t Reserved[3]; } ItemFlag;
typedef struct { int32_t Turn; } ItemTurn;
typedef struct { int32_t TopLeft, TopRight, BottomRight, BottomLeft; } ItemCorners;
typedef struct { uint16_t Role; uint16_t Reserved; } ItemRoleOf;
typedef struct { uint32_t Block; } ItemBlockOf;

/* Characters, or a caption: Bytes of UTF-8 that follow. */
typedef struct { uint32_t Bytes; } ItemBytes;

/* A colour: sRGB's numbers in sixteen bits each, and its opacity, not
 * multiplied in — #RRGGBBAA, each number times 257, with room to spare. */
typedef struct { uint16_t Red, Green, Blue, Alpha; } ItemColor;

/* A shape's outline: a rectangle of its size (its corners rounded by
 * ITEM_PROP_CORNERS), the ellipse in its size, or a path in points from its
 * corner — Steps step kinds, a byte each, padded to four, then the points
 * the steps take, an ItemPoint each. */
enum { ITEM_FORM_RECT = 0, ITEM_FORM_ELLIPSE = 1, ITEM_FORM_PATH = 2 };
enum { ITEM_STEP_MOVE = 1, ITEM_STEP_LINE = 2, ITEM_STEP_QUAD = 3, ITEM_STEP_CUBIC = 4, ITEM_STEP_CLOSE = 5 };
typedef struct {
    uint8_t  Form;
    uint8_t  Rule;        /* 0 non-zero, 1 even-odd */
    uint16_t Reserved;
    uint32_t Steps;
} ItemShape;

/* What an item is filled with: Count fills, each an ItemFill and what its
 * kind takes, one over another. */
enum {
    ITEM_FILL_COLOR   = 1,    /* ItemColor */
    ITEM_FILL_THEME   = 2,    /* ItemThemeColor: a colour of the theme's */
    ITEM_FILL_LINEAR  = 3,    /* ItemLinear, then Stops ItemStops */
    ITEM_FILL_RADIAL  = 4,    /* ItemRadial, then stops */
    ITEM_FILL_CONIC   = 5,    /* ItemConic, then stops */
    ITEM_FILL_PICTURE = 6,    /* ItemPictureFill */
};
enum { ITEM_EXTEND_PAD = 0, ITEM_EXTEND_REPEAT = 1, ITEM_EXTEND_REFLECT = 2, ITEM_EXTEND_NONE = 3 };
enum {
    ITEM_THEME_TEXT   = 1,    /* the text's colour */
    ITEM_THEME_PAGE   = 2,    /* the page's */
    ITEM_THEME_PANEL  = 3,
    ITEM_THEME_ACCENT = 4,
    ITEM_THEME_QUIET  = 5,    /* text said quietly */
    ITEM_THEME_COLORS = 6,
};
typedef struct {
    uint8_t  Kind;
    uint8_t  Extend;      /* gradients and pictures: past their ends (ITEM_EXTEND_*) */
    uint16_t Stops;       /* gradients */
    uint16_t Opacity;     /* the fill's own, ITEM_OPAQUE for whole */
    uint16_t Reserved;
} ItemFill;
typedef struct { uint16_t Which; uint16_t Reserved; } ItemThemeColor;
typedef struct { ItemPoint From, To; } ItemLinear;
typedef struct { ItemPoint Middle; int32_t Radius; ItemPoint Focus; } ItemRadial;
typedef struct { ItemPoint Middle; int32_t Start; } ItemConic;   /* Start in ITEM_TURN, 0 up */
typedef struct { uint16_t Offset; uint16_t Reserved; ItemColor Color; } ItemStop;   /* Offset of 65535 */
typedef struct {
    uint32_t  Block;
    ItemPoint At, Size;   /* where the picture's corner lies and how large it is drawn */
    uint8_t   Sharp;      /* its pixels kept square drawn larger */
    uint8_t   Reserved[3];
} ItemPictureFill;
typedef struct { uint16_t Count; uint16_t Reserved; } ItemFills;

/* A line along the outline: Dashes ITEM_POINT lengths follow, then one fill. */
enum { ITEM_CAP_BUTT = 0, ITEM_CAP_ROUND = 1, ITEM_CAP_SQUARE = 2 };
enum { ITEM_JOIN_MITER = 0, ITEM_JOIN_ROUND = 1, ITEM_JOIN_BEVEL = 2 };
enum { ITEM_ALIGN_CENTER = 0, ITEM_ALIGN_INSIDE = 1, ITEM_ALIGN_OUTSIDE = 2 };
typedef struct {
    int32_t  Width;
    uint8_t  Cap, Join, Align, Dashes;
    int32_t  MiterLimit;  /* of ITEM_POINT: 4 * 256 as SVG's */
    int32_t  DashOffset;
} ItemStroke;

/* Count shadows, raised or pressed (Inner), each ItemShadow. */
typedef struct {
    ItemPoint Offset;
    int32_t   Blur, Spread;
    ItemColor Color;
    uint8_t   Inner;
    uint8_t   Reserved[3];
} ItemShadow;
typedef struct { uint16_t Count; uint16_t Reserved; } ItemShadows;

/* How a text, or a stretch of it, is set: what Said says, and nothing else —
 * then FamilyBytes of UTF-8, the family's name or several apart by commas,
 * padded to four. */
enum {
    ITEM_SAYS_FAMILY      = 1,
    ITEM_SAYS_SIZE        = 2,
    ITEM_SAYS_WEIGHT      = 4,
    ITEM_SAYS_ITALIC      = 8,
    ITEM_SAYS_SPACING     = 16,
    ITEM_SAYS_LINE_HEIGHT = 32,
    ITEM_SAYS_UNDERLINE   = 64,
    ITEM_SAYS_STRIKE      = 128,
    ITEM_SAYS_COLOR       = 256,
};
typedef struct {
    uint16_t  Said;
    uint16_t  Weight;      /* 1 to 1000 */
    int32_t   Size;        /* of ITEM_POINT */
    int32_t   Spacing;     /* letter spacing, of ITEM_POINT */
    int32_t   LineHeight;  /* of ITEM_ONE times the size */
    uint8_t   Italic, Underline, Strike, Reserved;
    ItemColor Color;       /* a stretch's own colour, over the text's fills */
    uint16_t  FamilyBytes;
    uint16_t  Reserved2;
} ItemTextStyle;

/* Count stretches, each From, To in characters and an ItemTextStyle (with
 * its family's bytes) — a later one over an earlier where they meet. */
typedef struct { uint32_t From, To; } ItemSpanRange;
typedef struct { uint16_t Count; uint16_t Reserved; } ItemSpans;

enum { ITEM_TEXT_LEFT = 0, ITEM_TEXT_CENTER = 1, ITEM_TEXT_RIGHT = 2, ITEM_TEXT_JUSTIFY = 3 };
enum { ITEM_OVERFLOW_GROW = 0, ITEM_OVERFLOW_CLIP = 1, ITEM_OVERFLOW_ELLIPSIS = 2 };
typedef struct {
    uint8_t  Align, Overflow, Wrap, Reserved;
    uint32_t MaxLines;    /* 0: any */
} ItemTextBox;

/* A picture item's block, and how the picture fills the item's size. */
enum { ITEM_FIT_STRETCH = 0, ITEM_FIT_CONTAIN = 1, ITEM_FIT_COVER = 2, ITEM_FIT_CENTER = 3 };
typedef struct {
    uint32_t Block;
    uint8_t  Fit, Sharp;
    uint16_t Reserved;
} ItemPictureOf;

/* ---- Cells -------------------------------------------------------------- */

/* A cells item is a grid of cells in the console's look — its font, the size
 * of its cells — its size the grid's; where its place is unsaid it lies on
 * the console's grid, its first cell on the first cell corner of the grid in
 * its area, so a program of cells shows as the console does. A grid said
 * anew holds cells that show nothing; cells are written a run along a row at
 * a time (ITEM_OP_CELLS).
 *
 * A cell is a character in its ink on its paper, and a mark over it in a
 * colour of its own: a line under the glyph, or a bar down its left side —
 * so a caret a program draws stands on a letter and covers nothing. Its
 * colours are the console's (box/color.h): #RRGGBB; COLOR_DEFAULT, the
 * screen's own — its text colour for an ink, its page for a paper;
 * COLOR_THROUGH, what lies beneath; or a triple at an opacity over it. A cell
 * whose ink and paper are both COLOR_THROUGH and that has no mark shows
 * nothing. The console's own cells are these. */
enum { ITEM_MARK_NONE = 0, ITEM_MARK_UNDER = 1, ITEM_MARK_BAR = 2 };
typedef struct {
    uint32_t Character;   /* Unicode; 0 shows no character */
    uint32_t Ink;
    uint32_t Paper;
    uint32_t MarkInk;
    uint8_t  Mark;        /* ITEM_MARK_* */
    uint8_t  Reserved[3];
} ItemCell;

/* A cell that shows nothing: what a grid said anew holds. */
static inline ItemCell item_cell_empty(void)
{
    const ItemCell Empty = {0, COLOR_THROUGH, COLOR_THROUGH, 0, ITEM_MARK_NONE, {0, 0, 0}};
    return Empty;
}

typedef struct { uint32_t Cols, Rows; } ItemGrid;

/* The caret: drawn by the display at the foot of its cell while Shown. One
 * caret is on the glass — the topmost shown cells item's that shows one, else
 * the console's. */
typedef struct {
    uint32_t Col, Row;
    uint8_t  Shown;
    uint8_t  Reserved[3];
} ItemCaret;

/* Count cells written from Col of Row, an ItemCell each, after this head. */
typedef struct { uint32_t Row, Col, Count, Reserved; } ItemCellsRun;

/* Which grids finer than the halves the console's look draws a cell's
 * character in exactly — the quadrants (two across, two down), the sextants
 * (three down), the octants (four down), the characters of boxos_mosaic.h: a
 * cell of two colours on a grid it has is drawn as its pattern says, to the
 * pixel. Said with the area (DisplayAreaOpenReply, ItemAreaNews). */
enum { ITEM_MOSAIC_QUADRANTS = 1, ITEM_MOSAIC_SEXTANTS = 2, ITEM_MOSAIC_OCTANTS = 4 };

/* ---- Events: what comes back -------------------------------------------- */

typedef struct {
    uint16_t Kind;
    uint16_t Words;       /* eight-byte words, this head too */
    uint32_t Item;
} ItemEvent;

enum {
    ITEM_EVENT_PAD     = 0,
    ITEM_EVENT_AREA    = 1,   /* ItemAreaNews: the area's size, scale and glass, now */
    ITEM_EVENT_SIZE    = 2,   /* ItemPoint: an item sized by its content came to this */
    ITEM_EVENT_REFUSED = 3,   /* ItemRefusal: a change not made, and why */
    ITEM_EVENT_SHOWN   = 4,   /* ItemShown: a step on the glass, and what it cost */
    ITEM_EVENT_FREE    = 5,   /* ItemPageFree: a canvas's page may be drawn in again (Item is the block) */
    ITEM_EVENT_ANIMATION_END = 6,   /* ItemAnimationEnd: the last animation of a Number ended (Item is its item) */
};

/* The glass an area is on: a framebuffer's pixels, or a text page's cells —
 * where a cell is ITEM_PAGE_CELL_WIDE points across and ITEM_PAGE_CELL_TALL
 * down, a text is written into the cells a character each, a line of it a row,
 * and an item that is no text is shown by its caption. */
enum { ITEM_GLASS_TEXT = 1, ITEM_GLASS_PIXELS = 2 };

/* How the person wants things to move, told with the area and whenever it
 * changes: less motion — a program moving things itself moves them at once
 * and fades briefly too — and every animation's speed, in percent. */
enum { ITEM_MOTION_LESS = 1 };
#define ITEM_PAGE_CELL_WIDE 8
#define ITEM_PAGE_CELL_TALL 16
typedef struct {
    int32_t  Width, Height;   /* of ITEM_POINT */
    uint32_t Scale;           /* pixels to a point, of 256 */
    uint8_t  Glass;           /* ITEM_GLASS_* */
    uint8_t  Shown;           /* 0 while it lies behind and is not seen */
    uint8_t  Mosaics;         /* ITEM_MOSAIC_*: the grids cells are drawn in exactly */
    uint8_t  Motion;          /* ITEM_MOTION_* */
    uint32_t Speed;           /* percent */
    uint32_t Reserved;
} ItemAreaNews;

typedef struct {
    uint16_t Op, Property;
    int32_t  Reason;          /* error_t */
} ItemRefusal;

typedef struct {
    uint64_t Step;            /* the last step the glass shows */
    uint32_t Micros;          /* composing and putting it on the glass */
    uint32_t Pixels;          /* the pixels put on the glass — for every area's changes */
    uint32_t Bytes;           /* the ring's bytes it took */
    uint32_t Records;
    uint32_t Own;             /* of the pixels, those this area's own changes asked for */
    uint32_t Reserved;
} ItemShown;

typedef struct { uint32_t Page; uint32_t Reserved; } ItemPageFree;

typedef struct {
    uint32_t Number;
    uint8_t  Finished;        /* 1: every one of them ran to its end; 0: one was cut — said over, unsaid, or its item gone */
    uint8_t  Reserved[3];
} ItemAnimationEnd;

/* ---- The area's memory -------------------------------------------------- */

/* Its head, then the ring of changes, then the ring of events. Each cursor
 * counts bytes ever written or read and is on a line of its own: the program
 * moves Write and EventRead, the display Read and EventWrite. */
typedef struct {
    uint32_t Magic;           /* ITEM_AREA_MAGIC */
    uint32_t Bytes;           /* the ring of changes */
    uint32_t EventBytes;      /* the ring of events */
    uint32_t Lost;            /* events the display could not add: the ring was full */
    uint8_t  Pad0[48];
    uint64_t Write;
    uint8_t  Pad1[56];
    uint64_t Read;
    uint8_t  Pad2[56];
    uint64_t EventWrite;
    uint8_t  Pad3[56];
    uint64_t EventRead;
    uint8_t  Pad4[56];
} ItemAreaHeader;

#define ITEM_AREA_MAGIC    0x4D455449u   /* "ITEM" */
#define ITEM_RING_BYTES    (512u * 1024u)
#define ITEM_EVENT_BYTES   (64u * 1024u)

/* A record of `length` bytes — a whole number of words, at most half the
 * ring — placed in a ring of `bytes` at the writer's *write, the reader at
 * `read`: where to write it, or NULL when there is no room. A record never
 * crosses the ring's end: when it would, the words up to the end are a
 * record of ITEM_OP_PAD of no words — "to the end" — and the record goes at
 * the ring's start. *write is moved past it; the writer publishes it once the
 * record is written. Both rings of an area, and both their writers, keep it. */
uint8_t *item_ring_place(uint8_t *ring, uint32_t bytes, uint64_t *write, uint64_t read, uint32_t length);

/* ---- Blocks: pictures, canvases, fonts ---------------------------------- */

enum {
    ITEM_BLOCK_PICTURE = 1,   /* Width x Height pixels of R, G, B, A bytes, the opacity not multiplied in */
    ITEM_BLOCK_CANVAS  = 2,   /* two pages of Width x Height canvas pixels (painter/core/pixel.h) */
    ITEM_BLOCK_FONT    = 3,   /* a font file's Bytes */
};

typedef struct {
    uint32_t Magic;           /* ITEM_BLOCK_MAGIC */
    uint32_t Kind;
    uint32_t Width, Height;   /* pictures and canvases, in pixels */
    uint32_t Bytes;           /* a font's */
    uint32_t PageBytes;       /* a canvas page's */
    uint32_t PageFree[2];     /* a canvas's: 1 while the program may draw in that page */
    uint8_t  Pad[32];
} ItemBlockHeader;

#define ITEM_BLOCK_MAGIC   0x4B434C42u   /* "BLCK" */

/* ---- Bounds, per program over all its areas ------------------------------ */

#define ITEM_AREAS_MOST       16u
#define ITEM_ITEMS_MOST       65536u
#define ITEM_DEPTH_MOST       64u
#define ITEM_TEXT_MOST        (192u * 1024u)   /* a text's bytes: its record no more than half the ring */
#define ITEM_TEXTS_MOST       (16u * 1024u * 1024u)
#define ITEM_SPANS_MOST       4096u
#define ITEM_FILLS_MOST       16u
#define ITEM_STOPS_MOST       64u
#define ITEM_SHADOWS_MOST     8u
#define ITEM_DASHES_MOST      64u
#define ITEM_PATH_MOST        65536u           /* a path's steps */
#define ITEM_BLOCKS_MOST      1024u
#define ITEM_BLOCK_BYTES_MOST (64u * 1024u * 1024u)
#define ITEM_BLOCKS_BYTES_MOST (256u * 1024u * 1024u)
#define ITEM_REACH            (1 << 23)        /* of ITEM_POINT: no place or size past it */
#define ITEM_GRID_SIDE_MOST   4096u            /* a cells item's columns, and its rows */
#define ITEM_CELLS_MOST       (1u << 20)       /* cells, over all the program's cells items */
#define ITEM_ANIMATIONS_MOST  16384u           /* under way or waiting */
#define ITEM_ANIMATION_BYTES_MOST (16u * 1024u * 1024u)   /* the values they go to, together */
#define ITEM_TIME_MOST        3600000u         /* an animation's delay, and a run's time: an hour of milliseconds */

/* ---- Questions and messages --------------------------------------------- */

/* An area, asked: DisplayAreaOpenRequest → DisplayAreaOpenReply. It cares
 * where in the console's order it stands, so it carries the asker's
 * console_order_mark() (box/print.h): it is answered once everything the
 * asker printed before is drawn — the console's caret it names stands after
 * that, and an area in the flow takes the rows below it. A block, asked:
 * DisplayBlockOpenRequest → DisplayBlockOpenReply.
 *
 * An area's cells, folded into the console's text, asked:
 * DisplayAreaCommitRequest → DisplayAreaCommitReply. It carries the asker's
 * mark too, and lands there in the console's order: the console scrolled up
 * scroll_rows first, then every cell of the area's cells items that shows
 * something laid over the console's where it lies, the console's caret where
 * a shown caret of theirs stands, and what the cells say told into the log
 * ring. The area is gone then. An area that holds anything but cells items on
 * the console's grid is not folded, and stays (-ERR_UNSUPPORTED).
 *
 * Messages to the display that answered: AREA_STEP (DisplayAreaNotice) after
 * each step, AREA_CLOSE (DisplayAreaNotice), BLOCK_CLOSE (DisplayBlockNotice). */
#define DISP_CMD_AREA_OPEN    0x60
#define DISP_CMD_BLOCK_OPEN   0x61
#define DISP_CMD_AREA_COMMIT  0x62
#define DISP_CMD_AREA_STEP    0x68
#define DISP_CMD_AREA_CLOSE   0x69
#define DISP_CMD_BLOCK_CLOSE  0x6A

#define DISPLAY_AREA_TAG_PREFIX  "display:area:"
#define DISPLAY_BLOCK_TAG_PREFIX "display:block:"
#define DISPLAY_ITEM_TAG_MAX     48u

/* Where an area is wished to lie; the display decides. Bottom to top: the
 * console, the areas in its flow, those of the whole screen, those over
 * everything, and those shown always — each by its age. */
enum {
    DISPLAY_AREA_OVER  = 1,   /* over everything, as large as wished, where the display puts it */
    DISPLAY_AREA_WHOLE = 2,   /* the whole screen, while the program is in front */
    DISPLAY_AREA_FLOW  = 3,   /* in the console's flow from the caret's row, as wide as it, as tall as wished:
                               * the console makes room, its text goes on below, and the area moves with it */
};

/* An area's flags. ALWAYS: shown even while its program lives behind — a
 * clock in the corner, a companion — not only while it is in front. */
enum { DISPLAY_AREA_ALWAYS = 1 };

typedef struct PACKED {
    uint8_t  command;         /* DISP_CMD_AREA_OPEN */
    uint8_t  placement;       /* DISPLAY_AREA_* */
    uint16_t flags;           /* DISPLAY_AREA_ALWAYS, or 0 */
    int32_t  width;           /* wished, of ITEM_POINT; 0 for the display's choice */
    int32_t  height;
    uint32_t reserved;
    uint64_t order;           /* the asker's console_order_mark() */
} DisplayAreaOpenRequest;

typedef struct PACKED {
    uint8_t  command;         /* DISP_CMD_AREA_OPEN */
    uint8_t  glass;           /* ITEM_GLASS_* */
    uint8_t  mosaics;         /* ITEM_MOSAIC_*: the grids cells are drawn in exactly */
    uint8_t  motion;          /* ITEM_MOTION_* */
    int32_t  error;           /* error_t; the rest is meaningful only with 0 */
    uint32_t area;
    uint32_t display_pid;     /* where the messages go */
    int32_t  width;           /* given, of ITEM_POINT */
    int32_t  height;
    uint32_t scale;           /* pixels to a point, of 256 */
    uint32_t caret_col;       /* the console's caret, after everything the asker printed */
    uint32_t caret_row;
    uint32_t speed;           /* every animation's, in percent */
    char     bay_tag[DISPLAY_ITEM_TAG_MAX];
} DisplayAreaOpenReply;

typedef struct PACKED {
    uint8_t  command;         /* DISP_CMD_BLOCK_OPEN */
    uint8_t  kind;            /* ITEM_BLOCK_* */
    uint16_t reserved;
    uint32_t area;
    uint32_t width;           /* pictures and canvases */
    uint32_t height;
    uint32_t bytes;           /* a font's */
    uint32_t reserved2;
} DisplayBlockOpenRequest;

typedef struct PACKED {
    uint8_t  command;         /* DISP_CMD_BLOCK_OPEN */
    uint8_t  reserved[3];
    int32_t  error;
    uint32_t block;
    uint32_t reserved2;
    char     bay_tag[DISPLAY_ITEM_TAG_MAX];
} DisplayBlockOpenReply;

typedef struct PACKED {
    uint8_t  command;         /* DISP_CMD_AREA_COMMIT */
    uint8_t  reserved[3];
    uint32_t area;
    uint32_t scroll_rows;     /* the console scrolled up first, at most its rows */
    uint32_t reserved2;
    uint64_t order;           /* the asker's console_order_mark() */
} DisplayAreaCommitRequest;

typedef struct PACKED {
    uint8_t  command;         /* DISP_CMD_AREA_COMMIT */
    uint8_t  reserved[3];
    int32_t  error;           /* error_t: 0 — the cells are the console's text, and the area gone */
} DisplayAreaCommitReply;

typedef struct PACKED {
    uint8_t  command;         /* DISP_CMD_AREA_STEP or _CLOSE */
    uint8_t  reserved[3];
    uint32_t area;
    uint32_t generation;      /* the sender's */
} DisplayAreaNotice;

typedef struct PACKED {
    uint8_t  command;         /* DISP_CMD_BLOCK_CLOSE */
    uint8_t  reserved[3];
    uint32_t block;
    uint32_t generation;
} DisplayBlockNotice;

STATIC_ASSERT(sizeof(ItemRecord) == 8 && sizeof(ItemCreate) == 16 && sizeof(ItemChange) == 8 &&
                  sizeof(ItemBlockChanged) == 24 && sizeof(ItemStep) == 8 && sizeof(ItemAnimation) == 40,
              "the records of the items' language are an ABI of fixed sizes");
STATIC_ASSERT(sizeof(ItemColor) == 8 && sizeof(ItemFill) == 8 && sizeof(ItemStop) == 12 &&
                  sizeof(ItemTextStyle) == 32 && sizeof(ItemShadow) == 28 && sizeof(ItemStroke) == 16,
              "the values of the items' language are an ABI of fixed sizes");
STATIC_ASSERT(sizeof(ItemEvent) == 8 && sizeof(ItemAreaNews) == 24 && sizeof(ItemShown) == 32 &&
                  sizeof(ItemAnimationEnd) == 8,
              "the events of the items' language are an ABI of fixed sizes");
STATIC_ASSERT(sizeof(ItemAreaHeader) == 320 && sizeof(ItemBlockHeader) == 64,
              "an area's and a block's memory begin with heads of fixed sizes");
STATIC_ASSERT(sizeof(ItemCell) == 20 && sizeof(ItemGrid) == 8 && sizeof(ItemCaret) == 12 && sizeof(ItemCellsRun) == 16,
              "a cells item's values are an ABI of fixed sizes");
STATIC_ASSERT(sizeof(DisplayAreaOpenRequest) == 24 && sizeof(DisplayAreaOpenReply) == 88 &&
                  sizeof(DisplayAreaCommitRequest) == 24 && sizeof(DisplayAreaCommitReply) == 8 &&
                  sizeof(DisplayBlockOpenRequest) == 24 && sizeof(DisplayBlockOpenReply) == 64 &&
                  sizeof(DisplayAreaNotice) == 12 && sizeof(DisplayBlockNotice) == 12,
              "the questions and messages of areas and blocks are an ABI of fixed sizes");

#ifdef __cplusplus
}
#endif

#endif /* BOX_ITEMS_H */
