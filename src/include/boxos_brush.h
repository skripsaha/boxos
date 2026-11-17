/*
 * boxos_brush.h — the brush: pixels, for the hands that paint a picture
 * rather than cells. Gangway and the display paint a scene with it
 * (boxos_scene_play.h) and the host stand holds both to it
 * (tools/scene_stand.c) — one text, so a picture is the same pixels in the
 * loader, in the system and on the host.
 *
 * Whole numbers only: the loader runs with no SSE, and numbers worked the
 * same way everywhere make the same picture everywhere — the display goes on
 * from the very pixels the loader left. No memory is asked for: the caller
 * hands in every room the brush works in.
 *
 * A canvas is pixels in memory, 0x00RRGGBB, never the glass: video memory is
 * slow to read and a picture half made would show. What is made is shown
 * where it changed (BoxBrushShow), in the glass's layout.
 *
 * Every shape is an outline — contours of straight edges, the curves and the
 * round shapes broken into edges that stray less than an eighth of a pixel
 * where they land — filled by the share of each pixel it covers, worked out
 * from its edges exactly: the signed area each adds to the cells of a row. An
 * edge is as smooth at any slope, size and turn. A contour wound the other way
 * cuts a hole. Where solid contours overlap, a pixel either covers whole is
 * covered once; where only their edges share a pixel, their shares add, up to
 * the whole of it. A
 * paint gives each pixel its colour: one colour, a linear or a radial
 * gradient, a picture. Coverage, alpha and opacity make one share, and the
 * colour is laid over what is beneath by it, mixed as light (BoxColorOver,
 * boxos_color.h — the one way the kernel, the loader, the display and Painter
 * mix) — so text drawn here is the very pixels the press makes.
 *
 * Places are in 1/BOX_BRUSH_ONE of a pixel, a matrix's numbers in
 * 1/BOX_BRUSH_UNIT, angles in 1/BOX_BRUSH_TURN of a turn. No place of a
 * shape's own lies further than BOX_BRUSH_OWN_MOST from its 0,0, none lands
 * further than BOX_BRUSH_REACH pixels from the canvas's corner, and no matrix
 * stretches by more than BOX_BRUSH_STRETCH_MOST: an outline or a paint that
 * would is not drawn, and says so — never wrapped round into a wrong picture.
 *
 * The includer supplies uint8_t, uint16_t, uint32_t, int32_t, uint64_t,
 * int64_t and size_t.
 */
#ifndef BOXOS_BRUSH_H
#define BOXOS_BRUSH_H

#include "boxos_color.h"
#include "boxos_imprint.h"
#include "boxos_pixel.h"
#include "boxos_utf8.h"

#define BOX_BRUSH_ONE          256       /* a pixel, in places */
#define BOX_BRUSH_UNIT         65536     /* 1, in a matrix's numbers and a stop's place */
#define BOX_BRUSH_TURN         65536u    /* a whole turn */
#define BOX_BRUSH_REACH        32768     /* pixels: the furthest any place lies from the canvas's corner */
#define BOX_BRUSH_STRETCH_MOST 256       /* the most a matrix stretches a length by */
#define BOX_BRUSH_OWN_MOST     (1 << 26) /* places: the furthest a shape's own place or size reaches */
#define BOX_BRUSH_FULL         255u      /* a share that is whole */

/* How far a gradient runs in places of its own space: from its first stop at
 * 0 to its last here along x (LINEAR), or from its middle out to its last at
 * this radius (RADIAL) — a stop's place, so the way along is read as it is. */
#define BOX_BRUSH_GRADIENT BOX_BRUSH_UNIT

/* The furthest a contour strays from the curve it stands for, in places on
 * the canvas. */
#define BOX_BRUSH_FLAT (BOX_BRUSH_ONE / 8)

/* The most edges a round shape or a curve is broken into. */
#define BOX_BRUSH_STEPS_MOST 2048u

/* ---------------------------------------------------------------------------
 * Numbers
 * ------------------------------------------------------------------------- */

/* The whole number nearest below the square root of Value. */
static inline uint32_t BoxBrushRoot(uint32_t Value)
{
    static const uint8_t Fine[256] = {
         0,  1,  1,  1,  2,  2,  2,  2,  2,  3,  3,  3,  3,  3,  3,  3,
         4,  4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5,  5,  5,  5,  5,
         5,  5,  5,  5,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
         6,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,
         8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
         8,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,
         9,  9,  9,  9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
        10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11,
        11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
        12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
        12, 12, 12, 12, 12, 12, 12, 12, 12, 13, 13, 13, 13, 13, 13, 13,
        13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
        13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
        14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
        14, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
        15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15
    };
    static const uint8_t Coarse[256] = {
          0,  16,  22,  27,  32,  35,  39,  42,  45,  48,  50,  53,  55,  57,  59,  61,
         64,  65,  67,  69,  71,  73,  75,  76,  78,  80,  81,  83,  84,  86,  87,  89,
         90,  91,  93,  94,  96,  97,  98,  99, 101, 102, 103, 104, 106, 107, 108, 109,
        110, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126,
        128, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142,
        143, 144, 144, 145, 146, 147, 148, 149, 150, 150, 151, 152, 153, 154, 155, 155,
        156, 157, 158, 159, 160, 160, 161, 162, 163, 163, 164, 165, 166, 167, 167, 168,
        169, 170, 170, 171, 172, 173, 173, 174, 175, 176, 176, 177, 178, 178, 179, 180,
        181, 181, 182, 183, 183, 184, 185, 185, 186, 187, 187, 188, 189, 189, 190, 191,
        192, 192, 193, 193, 194, 195, 195, 196, 197, 197, 198, 199, 199, 200, 201, 201,
        202, 203, 203, 204, 204, 205, 206, 206, 207, 208, 208, 209, 209, 210, 211, 211,
        212, 212, 213, 214, 214, 215, 215, 216, 217, 217, 218, 218, 219, 219, 220, 221,
        221, 222, 222, 223, 224, 224, 225, 225, 226, 226, 227, 227, 228, 229, 229, 230,
        230, 231, 231, 232, 232, 233, 234, 234, 235, 235, 236, 236, 237, 237, 238, 238,
        239, 240, 240, 241, 241, 242, 242, 243, 243, 244, 244, 245, 245, 246, 246, 247,
        247, 248, 248, 249, 249, 250, 250, 251, 251, 252, 252, 253, 253, 254, 254, 255
    };
    if (Value < 256u) return Fine[Value];
    if (Value < 65536u) {
        uint32_t Root = Coarse[Value >> 8];
        while ((Root + 1u) * (Root + 1u) <= Value) Root++;
        return Root;
    }
    /* Past the tables: the root of Value's top fifteen or sixteen bits, an
     * even shift down, is the root's top bits — the middle of what they leave
     * open, one step of Newton's, and at most one to either side. */
    uint32_t Shift = 2u;
    while (Value >> Shift >= 65536u) Shift += 2u;
    uint32_t Root = (BoxBrushRoot(Value >> Shift) << (Shift / 2u)) + (1u << (Shift / 2u - 1u));
    Root = (Root + Value / Root) / 2u;
    if ((uint64_t)Root * Root > Value) Root--;
    else if ((uint64_t)(Root + 1u) * (Root + 1u) <= Value) Root++;
    return Root;
}

/* The whole number nearest below the square root of Value. */
static inline uint64_t BoxBrushRoot64(uint64_t Value)
{
    uint64_t Root = 0, Bit = 1ull << 62;
    while (Bit > Value) Bit >>= 2;
    while (Bit) {
        if (Value >= Root + Bit) {
            Value -= Root + Bit;
            Root = (Root >> 1) + Bit;
        } else {
            Root >>= 1;
        }
        Bit >>= 2;
    }
    return Root;
}

/* The sine of Turn, in 1/BOX_BRUSH_UNIT: a quarter of the wave kept, the rest
 * its mirror, so every quarter turn is exactly 0 or 1 and the wave is the same
 * on both sides of each. */
static inline int32_t BoxBrushSine(uint32_t Turn)
{
    static const int32_t Quarter[257] = {
                 0,    6588356,   13176464,   19764076,   26350943,   32936819,
          39521455,   46104602,   52686014,   59265442,   65842639,   72417357,
          78989349,   85558366,   92124163,   98686491,  105245103,  111799753,
         118350194,  124896179,  131437462,  137973796,  144504935,  151030634,
         157550647,  164064728,  170572633,  177074115,  183568930,  190056834,
         196537583,  203010932,  209476638,  215934457,  222384147,  228825464,
         235258165,  241682010,  248096755,  254502159,  260897982,  267283981,
         273659918,  280025552,  286380643,  292724951,  299058239,  305380268,
         311690799,  317989595,  324276419,  330551034,  336813204,  343062693,
         349299266,  355522689,  361732726,  367929144,  374111709,  380280190,
         386434353,  392573967,  398698801,  404808624,  410903207,  416982319,
         423045732,  429093217,  435124548,  441139496,  447137835,  453119340,
         459083786,  465030947,  470960600,  476872522,  482766489,  488642281,
         494499676,  500338453,  506158392,  511959275,  517740883,  523502998,
         529245404,  534967884,  540670223,  546352205,  552013618,  557654248,
         563273883,  568872310,  574449320,  580004702,  585538248,  591049748,
         596538995,  602005783,  607449906,  612871159,  618269338,  623644239,
         628995660,  634323400,  639627258,  644907034,  650162530,  655393548,
         660599890,  665781362,  670937767,  676068911,  681174602,  686254647,
         691308855,  696337036,  701339000,  706314559,  711263525,  716185713,
         721080937,  725949013,  730789757,  735602987,  740388522,  745146182,
         749875788,  754577161,  759250125,  763894504,  768510122,  773096806,
         777654384,  782182683,  786681534,  791150767,  795590213,  799999706,
         804379079,  808728167,  813046808,  817334838,  821592095,  825818421,
         830013654,  834177638,  838310216,  842411232,  846480531,  850517961,
         854523370,  858496606,  862437520,  866345964,  870221790,  874064853,
         877875009,  881652112,  885396022,  889106597,  892783698,  896427186,
         900036924,  903612776,  907154608,  910662286,  914135678,  917574653,
         920979082,  924348837,  927683790,  930983817,  934248793,  937478595,
         940673101,  943832191,  946955747,  950043650,  953095785,  956112036,
         959092290,  962036435,  964944360,  967815955,  970651112,  973449725,
         976211688,  978936898,  981625251,  984276646,  986890984,  989468165,
         992008094,  994510675,  996975812,  999403415, 1001793390, 1004145648,
        1006460100, 1008736660, 1010975242, 1013175761, 1015338134, 1017462281,
        1019548121, 1021595575, 1023604567, 1025575020, 1027506862, 1029400018,
        1031254418, 1033069992, 1034846671, 1036584389, 1038283080, 1039942680,
        1041563127, 1043144360, 1044686319, 1046188946, 1047652185, 1049075980,
        1050460278, 1051805027, 1053110176, 1054375676, 1055601479, 1056787540,
        1057933813, 1059040255, 1060106826, 1061133483, 1062120190, 1063066909,
        1063973603, 1064840240, 1065666786, 1066453210, 1067199483, 1067905576,
        1068571464, 1069197120, 1069782521, 1070327646, 1070832474, 1071296985,
        1071721163, 1072104991, 1072448455, 1072751542, 1073014240, 1073236540,
        1073418433, 1073559913, 1073660973, 1073721611, 1073741824
    };
    const uint32_t Quadrant = (Turn >> 14) & 3u;
    uint32_t       Into     = Turn & 16383u;
    if (Quadrant & 1u) Into = 16384u - Into;
    const uint32_t Step = Into >> 6, Part = Into & 63u;
    int64_t        Value = Quarter[Step];
    if (Part) Value += ((int64_t)(Quarter[Step + 1u] - Quarter[Step]) * Part) >> 6;
    const int32_t Sine = (int32_t)((Value + 8192) >> 14);
    return Quadrant >= 2u ? -Sine : Sine;
}

static inline int32_t BoxBrushCosine(uint32_t Turn)
{
    return BoxBrushSine(Turn + BOX_BRUSH_TURN / 4u);
}

static inline int64_t BoxBrushLeast(int64_t A, int64_t B) { return A < B ? A : B; }
static inline int64_t BoxBrushMost(int64_t A, int64_t B) { return A > B ? A : B; }
static inline int64_t BoxBrushSize(int64_t A) { return A < 0 ? -A : A; }

/* Value / UNIT to the nearest whole, halves away from 0 — the same on both
 * sides of 0, so a shape and its mirror land on mirrored places. */
static inline int64_t BoxBrushRound(int64_t Value)
{
    return Value >= 0 ? (Value + BOX_BRUSH_UNIT / 2) / BOX_BRUSH_UNIT : -((-Value + BOX_BRUSH_UNIT / 2) / BOX_BRUSH_UNIT);
}

/* Num / Den, Den above 0, to the nearest whole, halves away from 0. */
static inline int64_t BoxBrushRoundDiv(int64_t Num, int64_t Den)
{
    return Num >= 0 ? (Num + Den / 2) / Den : -((-Num + Den / 2) / Den);
}

/* The pixel a place lies in: Place / ONE, rounded down. */
static inline int64_t BoxBrushPixelOf(int64_t Place)
{
    return Place >= 0 ? Place / BOX_BRUSH_ONE : -((-Place + BOX_BRUSH_ONE - 1) / BOX_BRUSH_ONE);
}

/* ---------------------------------------------------------------------------
 * Rectangles, matrices
 * ------------------------------------------------------------------------- */

/* Whole pixels. */
typedef struct BoxBrushRect {
    int32_t X, Y, Width, Height;
} BoxBrushRect;

static inline BoxBrushRect BoxBrushMeet(BoxBrushRect A, BoxBrushRect B)
{
    const int64_t Left   = BoxBrushMost(A.X, B.X), Top = BoxBrushMost(A.Y, B.Y);
    const int64_t Right  = BoxBrushLeast((int64_t)A.X + A.Width, (int64_t)B.X + B.Width);
    const int64_t Bottom = BoxBrushLeast((int64_t)A.Y + A.Height, (int64_t)B.Y + B.Height);
    BoxBrushRect  Met    = { 0, 0, 0, 0 };
    if (Right <= Left || Bottom <= Top) return Met;
    Met.X      = (int32_t)Left;
    Met.Y      = (int32_t)Top;
    Met.Width  = (int32_t)(Right - Left);
    Met.Height = (int32_t)(Bottom - Top);
    return Met;
}

/* The least rectangle holding both; an empty one holds nothing. */
static inline BoxBrushRect BoxBrushJoin(BoxBrushRect A, BoxBrushRect B)
{
    if (A.Width <= 0 || A.Height <= 0) return B;
    if (B.Width <= 0 || B.Height <= 0) return A;
    const int64_t Left   = BoxBrushLeast(A.X, B.X), Top = BoxBrushLeast(A.Y, B.Y);
    const int64_t Right  = BoxBrushMost((int64_t)A.X + A.Width, (int64_t)B.X + B.Width);
    const int64_t Bottom = BoxBrushMost((int64_t)A.Y + A.Height, (int64_t)B.Y + B.Height);
    BoxBrushRect  Joined = { (int32_t)Left, (int32_t)Top, (int32_t)(Right - Left), (int32_t)(Bottom - Top) };
    return Joined;
}

/*
 * Where a place of a shape's or a paint's own space lands on the canvas —
 * SVG's matrix(a b c d e f):
 *     x' = (A·x + C·y) / UNIT + E        y' = (B·x + D·y) / UNIT + F
 * A to D in 1/UNIT, E and F places.
 */
typedef struct BoxBrushMatrix {
    int32_t A, B, C, D, E, F;
} BoxBrushMatrix;

/* A number kept to int32's room: a matrix that would not fit is one past what
 * the brush draws with, and is refused as one. */
static inline int32_t BoxBrushNarrow(int64_t Value)
{
    return Value > 2147483647ll ? 2147483647 : Value < -2147483647ll ? -2147483647 : (int32_t)Value;
}

/* Turned by Turn, stretched by Stretch (1/UNIT), its own 0,0 put at X, Y. */
static inline BoxBrushMatrix BoxBrushPlaced(int32_t X, int32_t Y, int32_t Stretch, uint32_t Turn)
{
    const int64_t  Cos = BoxBrushCosine(Turn), Sin = BoxBrushSine(Turn);
    BoxBrushMatrix M;
    M.A = BoxBrushNarrow(BoxBrushRound(Cos * Stretch));
    M.B = BoxBrushNarrow(BoxBrushRound(Sin * Stretch));
    M.C = -M.B;
    M.D = M.A;
    M.E = X;
    M.F = Y;
    return M;
}

/* M, and then N. */
static inline BoxBrushMatrix BoxBrushThen(BoxBrushMatrix M, BoxBrushMatrix N)
{
    BoxBrushMatrix R;
    R.A = BoxBrushNarrow(BoxBrushRound((int64_t)N.A * M.A + (int64_t)N.C * M.B));
    R.B = BoxBrushNarrow(BoxBrushRound((int64_t)N.B * M.A + (int64_t)N.D * M.B));
    R.C = BoxBrushNarrow(BoxBrushRound((int64_t)N.A * M.C + (int64_t)N.C * M.D));
    R.D = BoxBrushNarrow(BoxBrushRound((int64_t)N.B * M.C + (int64_t)N.D * M.D));
    R.E = BoxBrushNarrow(BoxBrushRound((int64_t)N.A * M.E + (int64_t)N.C * M.F) + N.E);
    R.F = BoxBrushNarrow(BoxBrushRound((int64_t)N.B * M.E + (int64_t)N.D * M.F) + N.F);
    return R;
}

/* Whether the brush draws with M: no number stretches past STRETCH_MOST and
 * its 0,0 lies within REACH. */
static inline int BoxBrushFits(const BoxBrushMatrix *M)
{
    const int64_t Most  = (int64_t)BOX_BRUSH_STRETCH_MOST * BOX_BRUSH_UNIT;
    const int64_t Reach = (int64_t)BOX_BRUSH_REACH * BOX_BRUSH_ONE;
    return BoxBrushSize(M->A) <= Most && BoxBrushSize(M->B) <= Most && BoxBrushSize(M->C) <= Most &&
           BoxBrushSize(M->D) <= Most && BoxBrushSize(M->E) <= Reach && BoxBrushSize(M->F) <= Reach;
}

/* The most M stretches a length by, in 1/UNIT — the longer of its two axes.
 * M fits. */
static inline int64_t BoxBrushStretchOf(const BoxBrushMatrix *M)
{
    const uint64_t X = (uint64_t)((int64_t)M->A * M->A + (int64_t)M->B * M->B);
    const uint64_t Y = (uint64_t)((int64_t)M->C * M->C + (int64_t)M->D * M->D);
    return (int64_t)BoxBrushRoot64(X > Y ? X : Y);
}

/* ---------------------------------------------------------------------------
 * The canvas, and laying a colour on it
 * ------------------------------------------------------------------------- */

typedef struct BoxBrushCanvas {
    uint32_t    *Pixels;   /* 0x00RRGGBB, row after row */
    uint32_t     Width;    /* pixels */
    uint32_t     Height;
    uint32_t     Stride;   /* pixels from one row to the next */
    BoxBrushRect Clip;     /* the only pixels drawn on */
    int32_t     *Row;      /* room for Width + 2 cells: the coverage of the row being filled */
} BoxBrushCanvas;

/* The pixels the canvas lets be drawn on: its clip, within it. */
static inline BoxBrushRect BoxBrushWithin(const BoxBrushCanvas *K)
{
    const BoxBrushRect Whole = { 0, 0, (int32_t)K->Width, (int32_t)K->Height };
    return BoxBrushMeet(K->Clip, Whole);
}

/* Coverage, alpha and opacity, each of 255, made one share of 255. */
static inline uint32_t BoxBrushShare(uint32_t Coverage, uint32_t Alpha, uint32_t Opacity)
{
    return (Coverage * Alpha * Opacity + 32512u) / 65025u;
}

/* Rect, within the clip, made one colour — 0x00RRGGBB, whole. */
static inline void BoxBrushClear(const BoxBrushCanvas *K, BoxBrushRect Rect, uint32_t Color)
{
    const BoxBrushRect At = BoxBrushMeet(Rect, BoxBrushWithin(K));
    for (int32_t Y = 0; Y < At.Height; Y++) {
        uint32_t *Line = K->Pixels + (size_t)(At.Y + Y) * K->Stride + At.X;
        for (int32_t X = 0; X < At.Width; X++) Line[X] = Color & 0x00FFFFFFu;
    }
}

/* One pixel: Color, 0xAARRGGBB, laid over it at Opacity. */
static inline void BoxBrushPixel(const BoxBrushCanvas *K, int32_t X, int32_t Y, uint32_t Color, uint32_t Opacity)
{
    const BoxBrushRect At = BoxBrushWithin(K);
    if (X < At.X || Y < At.Y || X >= At.X + At.Width || Y >= At.Y + At.Height) return;
    const uint32_t Share = BoxBrushShare(BOX_BRUSH_FULL, Color >> 24, Opacity);
    uint32_t      *Pixel = K->Pixels + (size_t)Y * K->Stride + X;
    if (Share) *Pixel = BoxColorOver(*Pixel, Color, Share);
}

/* Rect of the canvas onto the glass whose first byte is Glass — as many
 * pixels as the canvas, Stride bytes from row to row, four a pixel, in the
 * layout Format (BOX_PIXEL_*). The glass is written, never read. */
static inline void BoxBrushShow(const BoxBrushCanvas *K, BoxBrushRect Rect, volatile uint8_t *Glass, uint32_t Stride,
                                uint32_t Format)
{
    const BoxBrushRect Whole = { 0, 0, (int32_t)K->Width, (int32_t)K->Height };
    const BoxBrushRect At    = BoxBrushMeet(Rect, Whole);
    for (int32_t Y = 0; Y < At.Height; Y++) {
        const uint32_t    *From = K->Pixels + (size_t)(At.Y + Y) * K->Stride + At.X;
        volatile uint32_t *To   = (volatile uint32_t *)(Glass + (size_t)(At.Y + Y) * Stride) + At.X;
        for (int32_t X = 0; X < At.Width; X++) To[X] = BoxPixelEncode(From[X], Format);
    }
}

/* ---------------------------------------------------------------------------
 * Outlines
 * ------------------------------------------------------------------------- */

/* An edge on the canvas, in places, from its first end to its second — the
 * way it runs is the way its contour winds. Never level: a level edge covers
 * nothing. */
typedef struct BoxBrushEdge {
    int32_t X0, Y0, X1, Y1;
} BoxBrushEdge;

/*
 * An outline being drawn: the edges its contours have made so far, into the
 * caller's room, every place drawn through Matrix onto the canvas. Spilled:
 * an edge found no room, a place landed past REACH, the matrix does not fit,
 * or an edge was drawn with no contour begun — it is not filled.
 */
typedef struct BoxBrushOutline {
    BoxBrushEdge  *Edges;
    uint32_t       Room;
    uint32_t       Count;
    BoxBrushMatrix Matrix;
    int64_t        Stretch;                   /* the most Matrix stretches a length by, 1/UNIT */
    int32_t        Left, Top, Right, Bottom;  /* the places every edge lies within */
    int32_t        StartX, StartY;            /* the contour's first place, on the canvas */
    int32_t        AtX, AtY;                  /* the pen */
    int            Open;                      /* a contour is begun and not closed */
    int            Spilled;
} BoxBrushOutline;

static inline void BoxBrushBegin(BoxBrushOutline *O, BoxBrushEdge *Edges, uint32_t Room, const BoxBrushMatrix *Matrix)
{
    O->Edges   = Edges;
    O->Room    = Room;
    O->Count   = 0;
    O->Matrix  = *Matrix;
    O->Left    = O->Top = 2147483647;
    O->Right   = O->Bottom = -2147483647;
    O->StartX  = O->StartY = O->AtX = O->AtY = 0;
    O->Open    = 0;
    O->Spilled = !BoxBrushFits(Matrix);
    O->Stretch = O->Spilled ? 0 : BoxBrushStretchOf(Matrix);
}

/* Whether every one of Count places or sizes of a shape's own lies within
 * OWN_MOST; the outline spilled when one does not. */
static inline int BoxBrushOwn(BoxBrushOutline *O, const int32_t *Values, uint32_t Count)
{
    for (uint32_t I = 0; I < Count; I++)
        if (BoxBrushSize(Values[I]) > BOX_BRUSH_OWN_MOST) O->Spilled = 1;
    return !O->Spilled;
}

/* X, Y of the outline's own space, on the canvas: 0 when it lies past
 * OWN_MOST or lands past REACH. */
static inline int BoxBrushLand(BoxBrushOutline *O, int32_t X, int32_t Y, int32_t *CanvasX, int32_t *CanvasY)
{
    const int32_t Own[2] = { X, Y };
    if (!BoxBrushOwn(O, Own, 2)) return 0;
    const BoxBrushMatrix *M     = &O->Matrix;
    const int64_t         Reach = (int64_t)BOX_BRUSH_REACH * BOX_BRUSH_ONE;
    const int64_t         MX    = BoxBrushRound((int64_t)M->A * X + (int64_t)M->C * Y) + M->E;
    const int64_t         MY    = BoxBrushRound((int64_t)M->B * X + (int64_t)M->D * Y) + M->F;
    if (BoxBrushSize(MX) > Reach || BoxBrushSize(MY) > Reach) {
        O->Spilled = 1;
        return 0;
    }
    *CanvasX = (int32_t)MX;
    *CanvasY = (int32_t)MY;
    return 1;
}

/* An edge from the pen to X, Y on the canvas. */
static inline void BoxBrushEdgeTo(BoxBrushOutline *O, int32_t X, int32_t Y)
{
    if (Y != O->AtY) {
        if (O->Count == O->Room) {
            O->Spilled = 1;
        } else {
            BoxBrushEdge *E = &O->Edges[O->Count++];
            E->X0           = O->AtX;
            E->Y0           = O->AtY;
            E->X1           = X;
            E->Y1           = Y;
            O->Left         = (int32_t)BoxBrushLeast(O->Left, BoxBrushLeast(X, O->AtX));
            O->Right        = (int32_t)BoxBrushMost(O->Right, BoxBrushMost(X, O->AtX));
            O->Top          = (int32_t)BoxBrushLeast(O->Top, BoxBrushLeast(Y, O->AtY));
            O->Bottom       = (int32_t)BoxBrushMost(O->Bottom, BoxBrushMost(Y, O->AtY));
        }
    }
    O->AtX = X;
    O->AtY = Y;
}

static inline void BoxBrushClose(BoxBrushOutline *O)
{
    if (!O->Open || O->Spilled) return;
    BoxBrushEdgeTo(O, O->StartX, O->StartY);
    O->Open = 0;
}

/* A new contour begun at X, Y; the one open closed first. */
static inline void BoxBrushMoveTo(BoxBrushOutline *O, int32_t X, int32_t Y)
{
    int32_t CX, CY;
    BoxBrushClose(O);
    if (O->Spilled || !BoxBrushLand(O, X, Y, &CX, &CY)) return;
    O->StartX = O->AtX = CX;
    O->StartY = O->AtY = CY;
    O->Open            = 1;
}

/* The pen, and the contour's place, before an edge is drawn: none begun
 * spills the outline. */
static inline int BoxBrushPen(BoxBrushOutline *O)
{
    if (O->Spilled) return 0;
    if (!O->Open) {
        O->Spilled = 1;
        return 0;
    }
    return 1;
}

static inline void BoxBrushLineTo(BoxBrushOutline *O, int32_t X, int32_t Y)
{
    int32_t CX, CY;
    if (BoxBrushPen(O) && BoxBrushLand(O, X, Y, &CX, &CY)) BoxBrushEdgeTo(O, CX, CY);
}

/* How many edges a curve whose control points bend Bend places away from
 * straight is broken into, the bend times Factor quarters of the stray an
 * edge may leave (a quadratic 1, a cubic 3). */
static inline uint32_t BoxBrushCurveSteps(int64_t Bend, uint32_t Factor)
{
    const uint64_t Steps = BoxBrushRoot64((uint64_t)Bend * Factor / (4u * BOX_BRUSH_FLAT)) + 1u;
    return Steps > BOX_BRUSH_STEPS_MOST ? BOX_BRUSH_STEPS_MOST : (uint32_t)Steps;
}

/* How far apart, in places, two places are: the bend of a curve. */
static inline int64_t BoxBrushApart(int64_t X, int64_t Y)
{
    return (int64_t)BoxBrushRoot64((uint64_t)(X * X + Y * Y));
}

static inline int64_t BoxBrushLerp(int64_t A, int64_t B, int64_t T)
{
    return A + BoxBrushRound((B - A) * T);
}

/* A quadratic curve from the pen through the pull of CX, CY to X, Y. */
static inline void BoxBrushQuadTo(BoxBrushOutline *O, int32_t CX, int32_t CY, int32_t X, int32_t Y)
{
    int32_t PX, PY, EX, EY;
    if (!BoxBrushPen(O) || !BoxBrushLand(O, CX, CY, &PX, &PY) || !BoxBrushLand(O, X, Y, &EX, &EY)) return;
    const int64_t  SX = O->AtX, SY = O->AtY;
    const uint32_t N  = BoxBrushCurveSteps(BoxBrushApart(SX - 2 * PX + EX, SY - 2 * PY + EY), 1u);
    for (uint32_t I = 1; I < N; I++) {
        const int64_t T = (int64_t)I * BOX_BRUSH_UNIT / N;
        BoxBrushEdgeTo(O, (int32_t)BoxBrushLerp(BoxBrushLerp(SX, PX, T), BoxBrushLerp(PX, EX, T), T),
                       (int32_t)BoxBrushLerp(BoxBrushLerp(SY, PY, T), BoxBrushLerp(PY, EY, T), T));
    }
    BoxBrushEdgeTo(O, EX, EY);
}

/* A cubic curve from the pen, pulled by C1 and C2, to X, Y. */
static inline void BoxBrushCubicTo(BoxBrushOutline *O, int32_t C1X, int32_t C1Y, int32_t C2X, int32_t C2Y, int32_t X,
                                   int32_t Y)
{
    int32_t P1X, P1Y, P2X, P2Y, EX, EY;
    if (!BoxBrushPen(O) || !BoxBrushLand(O, C1X, C1Y, &P1X, &P1Y) || !BoxBrushLand(O, C2X, C2Y, &P2X, &P2Y) ||
        !BoxBrushLand(O, X, Y, &EX, &EY))
        return;
    const int64_t  SX   = O->AtX, SY = O->AtY;
    const int64_t  Bend = BoxBrushMost(BoxBrushApart(SX - 2 * P1X + P2X, SY - 2 * P1Y + P2Y),
                                       BoxBrushApart(P1X - 2 * P2X + EX, P1Y - 2 * P2Y + EY));
    const uint32_t N    = BoxBrushCurveSteps(Bend, 3u);
    for (uint32_t I = 1; I < N; I++) {
        const int64_t T   = (int64_t)I * BOX_BRUSH_UNIT / N;
        const int64_t AX  = BoxBrushLerp(SX, P1X, T), AY = BoxBrushLerp(SY, P1Y, T);
        const int64_t BX  = BoxBrushLerp(P1X, P2X, T), BY = BoxBrushLerp(P1Y, P2Y, T);
        const int64_t CX  = BoxBrushLerp(P2X, EX, T), CY = BoxBrushLerp(P2Y, EY, T);
        const int64_t ABX = BoxBrushLerp(AX, BX, T), ABY = BoxBrushLerp(AY, BY, T);
        const int64_t BCX = BoxBrushLerp(BX, CX, T), BCY = BoxBrushLerp(BY, CY, T);
        BoxBrushEdgeTo(O, (int32_t)BoxBrushLerp(ABX, BCX, T), (int32_t)BoxBrushLerp(ABY, BCY, T));
    }
    BoxBrushEdgeTo(O, EX, EY);
}

/* How many edges a round shape of Radius places on the canvas is broken into:
 * each strays at most FLAT from the curve, a multiple of eight so the shape
 * is its own mirror across both axes and both diagonals, eight at the
 * fewest. */
static inline uint32_t BoxBrushRoundSteps(int64_t Radius)
{
    /* A turn of n edges strays Radius·(2π/n)²/8: n = 2π·√(Radius / 8·FLAT). */
    const uint64_t Root  = BoxBrushRoot64((uint64_t)BoxBrushMost(Radius, 0) * 65536u / (8u * BOX_BRUSH_FLAT));
    uint64_t       Steps = (Root * 1608u + 65535u) >> 16;   /* 1608/256 is 2π */
    Steps                = (Steps + 7u) & ~7ull;
    return Steps < 8u ? 8u : Steps > BOX_BRUSH_STEPS_MOST ? BOX_BRUSH_STEPS_MOST : (uint32_t)Steps;
}

/* The turn of step Step of Steps along an arc of Span: the steps from either
 * end the same — Step and Steps - Step add up to Span exactly — so an arc is
 * its own mirror about its middle. */
static inline uint32_t BoxBrushArcTurn(uint32_t Step, uint32_t Steps, uint32_t Span)
{
    if (2u * Step <= Steps) return (uint32_t)((uint64_t)Step * Span / Steps);
    return Span - (uint32_t)((uint64_t)(Steps - Step) * Span / Steps);
}

/* The length, on the canvas, of Length places of the outline's own space at
 * its longest. */
static inline int64_t BoxBrushOnCanvas(const BoxBrushOutline *O, int64_t Length)
{
    return (BoxBrushSize(Length) * O->Stretch) >> 16;
}

/* Every shape below is wound the one way — down the right side on the canvas
 * as its own space stands — unless it is a Hole, wound the other. */

static inline void BoxBrushAddRect(BoxBrushOutline *O, int32_t X, int32_t Y, int32_t Width, int32_t Height, int Hole)
{
    const int32_t Own[4] = { X, Y, Width, Height };
    if (!BoxBrushOwn(O, Own, 4)) return;
    const int32_t Xs[4] = { X, X + Width, X + Width, X };
    const int32_t Ys[4] = { Y, Y, Y + Height, Y + Height };
    for (uint32_t I = 0; I < 4u; I++) {
        const uint32_t K = Hole ? 3u - I : I;
        if (I == 0) BoxBrushMoveTo(O, Xs[K], Ys[K]);
        else        BoxBrushLineTo(O, Xs[K], Ys[K]);
    }
    BoxBrushClose(O);
}

/* Each quarter of the ellipse from the same quarter of a circle, turned: the
 * shape is exactly its own mirror. Its corners lie a little outside the
 * ellipse — by √(θ / sin θ), θ the turn of an edge — so the edges cross the
 * curve and cover what it covers, neither less nor more. */
static inline void BoxBrushAddEllipse(BoxBrushOutline *O, int32_t CX, int32_t CY, int32_t RX, int32_t RY, int Hole)
{
    const int32_t Own[4] = { CX, CY, RX, RY };
    if (!BoxBrushOwn(O, Own, 4)) return;
    const uint32_t N     = BoxBrushRoundSteps(BoxBrushOnCanvas(O, BoxBrushMost(BoxBrushSize(RX), BoxBrushSize(RY))));
    const uint32_t Q     = N / 4u;
    const int64_t  Edge  = 411775 / N;   /* θ in 1/UNIT: 2π is 411775 */
    const int64_t  Out   = (int64_t)BoxBrushRoot64((uint64_t)(Edge * BOX_BRUSH_UNIT / BoxBrushSine(BOX_BRUSH_TURN / N)) *
                                                   BOX_BRUSH_UNIT);
    const int64_t  WideX = BoxBrushRound(BoxBrushSize(RX) * Out), WideY = BoxBrushRound(BoxBrushSize(RY) * Out);
    for (uint32_t I = 0; I < N; I++) {
        const uint32_t K       = Hole ? (N - I) % N : I;
        const uint32_t Quarter = K / Q;
        const uint32_t Turn    = BoxBrushArcTurn(K % Q, Q, BOX_BRUSH_TURN / 4u);
        const int64_t  Cos     = BoxBrushCosine(Turn), Sin = BoxBrushSine(Turn);
        /* The quarter's offsets worked unsigned, then given their signs. */
        const int64_t  OX      = BoxBrushRound(WideX * (Quarter & 1u ? Sin : Cos));
        const int64_t  OY      = BoxBrushRound(WideY * (Quarter & 1u ? Cos : Sin));
        const int32_t  X       = (int32_t)(CX + (Quarter == 1u || Quarter == 2u ? -OX : OX));
        const int32_t  Y       = (int32_t)(CY + (Quarter >= 2u ? -OY : OY));
        if (I == 0) BoxBrushMoveTo(O, X, Y);
        else        BoxBrushLineTo(O, X, Y);
    }
    BoxBrushClose(O);
}

/* A rectangle whose corners are quarters of a circle of Radius — no more than
 * half its shorter side. */
static inline void BoxBrushAddRounded(BoxBrushOutline *O, int32_t X, int32_t Y, int32_t Width, int32_t Height,
                                      int32_t Radius, int Hole)
{
    const int32_t Own[5] = { X, Y, Width, Height, Radius };
    if (!BoxBrushOwn(O, Own, 5)) return;
    const int32_t R = (int32_t)BoxBrushLeast(BoxBrushMost(Radius, 0), BoxBrushLeast(Width, Height) / 2);
    if (R == 0) {
        BoxBrushAddRect(O, X, Y, Width, Height, Hole);
        return;
    }
    const uint32_t Q        = BoxBrushRoundSteps(BoxBrushOnCanvas(O, R)) / 4u;
    const uint32_t Points   = 4u * (Q + 1u);
    const int32_t  Xs[4]    = { X + Width - R, X + Width - R, X + R, X + R };
    const int32_t  Ys[4]    = { Y + R, Y + Height - R, Y + Height - R, Y + R };
    for (uint32_t I = 0; I < Points; I++) {
        const uint32_t K      = Hole ? Points - 1u - I : I;
        const uint32_t Corner = K / (Q + 1u), Step = K % (Q + 1u);
        const uint32_t Turn   = (3u + Corner) * (BOX_BRUSH_TURN / 4u) + BoxBrushArcTurn(Step, Q, BOX_BRUSH_TURN / 4u);
        const int32_t  PX     = (int32_t)(Xs[Corner] + BoxBrushRound((int64_t)R * BoxBrushCosine(Turn)));
        const int32_t  PY     = (int32_t)(Ys[Corner] + BoxBrushRound((int64_t)R * BoxBrushSine(Turn)));
        if (I == 0) BoxBrushMoveTo(O, PX, PY);
        else        BoxBrushLineTo(O, PX, PY);
    }
    BoxBrushClose(O);
}

/* A line Width wide from X0, Y0 to X1, Y1: its ends cut square across, or
 * Round — half a circle each, and a line of no length a circle. Lines drawn
 * into one outline join without a seam. */
static inline void BoxBrushAddLine(BoxBrushOutline *O, int32_t X0, int32_t Y0, int32_t X1, int32_t Y1, int32_t Width,
                                   int Round)
{
    const int32_t Own[5] = { X0, Y0, X1, Y1, Width };
    if (!BoxBrushOwn(O, Own, 5)) return;
    const int64_t Half = BoxBrushMost(Width, 0) / 2;
    const int64_t DX = (int64_t)X1 - X0, DY = (int64_t)Y1 - Y0;
    const int64_t Length = BoxBrushApart(DX, DY);
    if (Half == 0) return;
    if (Length == 0) {
        if (Round) BoxBrushAddEllipse(O, X0, Y0, (int32_t)Half, (int32_t)Half, 0);
        return;
    }
    /* Along the line and across it, each Half long, to the nearest place. */
    const int64_t AX = (2 * DX * Half + (DX < 0 ? -Length : Length)) / (2 * Length);
    const int64_t AY = (2 * DY * Half + (DY < 0 ? -Length : Length)) / (2 * Length);
    const int64_t NX = -AY, NY = AX;
    const uint32_t Cap = Round ? BoxBrushRoundSteps(BoxBrushOnCanvas(O, Half)) / 2u : 0u;
    BoxBrushMoveTo(O, (int32_t)(X0 - NX), (int32_t)(Y0 - NY));
    BoxBrushLineTo(O, (int32_t)(X1 - NX), (int32_t)(Y1 - NY));
    for (uint32_t I = 1; I < Cap; I++) {
        const uint32_t Turn = BoxBrushArcTurn(I, Cap, BOX_BRUSH_TURN / 2u);
        const int64_t  Cos = BoxBrushCosine(Turn), Sin = BoxBrushSine(Turn);
        BoxBrushLineTo(O, (int32_t)(X1 + BoxBrushRound(-NX * Cos + AX * Sin)), (int32_t)(Y1 + BoxBrushRound(-NY * Cos + AY * Sin)));
    }
    BoxBrushLineTo(O, (int32_t)(X1 + NX), (int32_t)(Y1 + NY));
    BoxBrushLineTo(O, (int32_t)(X0 + NX), (int32_t)(Y0 + NY));
    for (uint32_t I = 1; I < Cap; I++) {
        const uint32_t Turn = BoxBrushArcTurn(I, Cap, BOX_BRUSH_TURN / 2u);
        const int64_t  Cos = BoxBrushCosine(Turn), Sin = BoxBrushSine(Turn);
        BoxBrushLineTo(O, (int32_t)(X0 + BoxBrushRound(NX * Cos - AX * Sin)), (int32_t)(Y0 + BoxBrushRound(NY * Cos - AY * Sin)));
    }
    BoxBrushClose(O);
}

/* ---------------------------------------------------------------------------
 * Paints
 * ------------------------------------------------------------------------- */

#define BOX_BRUSH_PAINT_SOLID   0u
#define BOX_BRUSH_PAINT_LINEAR  1u
#define BOX_BRUSH_PAINT_RADIAL  2u
#define BOX_BRUSH_PAINT_PICTURE 3u

/* A gradient's colour, 0xAARRGGBB, from At (0 to UNIT) of its way on. */
typedef struct BoxBrushStop {
    uint32_t At;
    uint32_t Color;
} BoxBrushStop;

/*
 * What colour a pixel is given. SOLID: Color, 0xAARRGGBB. LINEAR and RADIAL:
 * StopCount stops, At rising, at least one, the colours between walked
 * straight (BoxColorBetween), the first before the first stop and the last
 * past the last;
 * Space lays the gradient's own space (BOX_BRUSH_GRADIENT) on the canvas.
 * PICTURE: PictureWidth x PictureHeight pixels, 0xAARRGGBB, PictureStride
 * pixels from row to row, the picture's pixel I, J covering places I·ONE to
 * (I+1)·ONE of its space across and J·ONE to (J+1)·ONE down, laid by Space;
 * between the middles of its pixels the picture is smooth, and past its edge
 * clear.
 */
typedef struct BoxBrushPaint {
    uint32_t            Kind;
    uint32_t            Color;
    const BoxBrushStop *Stops;
    uint32_t            StopCount;
    BoxBrushMatrix      Space;
    const uint32_t     *Picture;
    uint32_t            PictureWidth;
    uint32_t            PictureHeight;
    uint32_t            PictureStride;
} BoxBrushPaint;

/* A paint made ready for a fill: the canvas onto its space — u = (IA·dx +
 * IC·dy) / UNIT, v = (IB·dx + ID·dy) / UNIT, dx and dy a pixel's middle less
 * the space's 0,0. */
typedef struct BoxBrushInk {
    const BoxBrushPaint *Paint;
    int64_t              IA, IB, IC, ID;
} BoxBrushInk;

/* 0 when the paint's space does not fit, folds flat or shrinks past what the
 * brush undoes. */
static inline int BoxBrushInkOf(const BoxBrushPaint *P, BoxBrushInk *Ink)
{
    Ink->Paint = P;
    Ink->IA = Ink->IB = Ink->IC = Ink->ID = 0;
    if (P->Kind == BOX_BRUSH_PAINT_SOLID) return 1;
    if (P->Kind > BOX_BRUSH_PAINT_PICTURE) return 0;
    if (P->Kind == BOX_BRUSH_PAINT_PICTURE ? !P->Picture || !P->PictureWidth || !P->PictureHeight : !P->StopCount)
        return 0;
    const BoxBrushMatrix *M = &P->Space;
    if (!BoxBrushFits(M)) return 0;
    const int64_t Folds = (int64_t)M->A * M->D - (int64_t)M->B * M->C;
    if (Folds == 0) return 0;
    Ink->IA = ((int64_t)M->D * 4294967296ll) / Folds;
    Ink->IB = ((int64_t)-M->B * 4294967296ll) / Folds;
    Ink->IC = ((int64_t)-M->C * 4294967296ll) / Folds;
    Ink->ID = ((int64_t)M->A * 4294967296ll) / Folds;
    const int64_t Most = 2147483647ll;
    return BoxBrushSize(Ink->IA) <= Most && BoxBrushSize(Ink->IB) <= Most && BoxBrushSize(Ink->IC) <= Most &&
           BoxBrushSize(Ink->ID) <= Most;
}

/* The gradient's colour At (0 to UNIT) of its way. */
static inline uint32_t BoxBrushAlong(const BoxBrushPaint *P, uint32_t At)
{
    const BoxBrushStop *S = P->Stops;
    if (At <= S[0].At) return S[0].Color;
    for (uint32_t K = 1; K < P->StopCount; K++) {
        if (At > S[K].At) continue;
        const uint32_t Span = S[K].At - S[K - 1u].At;
        if (Span == 0) return S[K].Color;
        return BoxColorBetween(S[K - 1u].Color, S[K].Color, (uint32_t)(((uint64_t)(At - S[K - 1u].At) * BOX_COLOR_WAY) / Span));
    }
    return S[P->StopCount - 1u].Color;
}

/* The picture's pixel I, J — clear past its edge. */
static inline uint32_t BoxBrushTexel(const BoxBrushPaint *P, int64_t I, int64_t J)
{
    if (I < 0 || J < 0 || I >= P->PictureWidth || J >= P->PictureHeight) return 0;
    return P->Picture[(size_t)J * P->PictureStride + (size_t)I];
}

/* The picture at U, V of its space: the four pixels whose middles lie round
 * it, weighed by nearness, each by its alpha — so a clear pixel lends no
 * colour to the edge of one beside it. */
static inline uint32_t BoxBrushSample(const BoxBrushPaint *P, int64_t U, int64_t V)
{
    U -= BOX_BRUSH_ONE / 2;
    V -= BOX_BRUSH_ONE / 2;
    const int64_t  I = BoxBrushPixelOf(U), J = BoxBrushPixelOf(V);
    const uint32_t FX = (uint32_t)(U - I * BOX_BRUSH_ONE), FY = (uint32_t)(V - J * BOX_BRUSH_ONE);
    const uint32_t Texels[4]  = { BoxBrushTexel(P, I, J), BoxBrushTexel(P, I + 1, J), BoxBrushTexel(P, I, J + 1),
                                  BoxBrushTexel(P, I + 1, J + 1) };
    const uint32_t Weights[4] = { (256u - FX) * (256u - FY), FX * (256u - FY), (256u - FX) * FY, FX * FY };
    uint64_t       Alpha = 0, Channels[3] = { 0, 0, 0 };
    for (uint32_t K = 0; K < 4u; K++) {
        const uint64_t Held = (uint64_t)Weights[K] * (Texels[K] >> 24);
        Alpha += Held;
        for (uint32_t C = 0; C < 3u; C++) Channels[C] += Held * ((Texels[K] >> (8u * C)) & 0xFFu);
    }
    if (Alpha == 0) return 0;
    uint32_t Color = (uint32_t)((Alpha + 32768u) >> 16) << 24;
    for (uint32_t C = 0; C < 3u; C++) Color |= (uint32_t)((Channels[C] + Alpha / 2u) / Alpha) << (8u * C);
    return Color;
}

/* The paint's colour, 0xAARRGGBB, at pixel X, Y's middle. */
static inline uint32_t BoxBrushInkAt(const BoxBrushInk *Ink, int32_t X, int32_t Y)
{
    const BoxBrushPaint *P = Ink->Paint;
    if (P->Kind == BOX_BRUSH_PAINT_SOLID) return P->Color;
    const int64_t DX = (int64_t)X * BOX_BRUSH_ONE + BOX_BRUSH_ONE / 2 - P->Space.E;
    const int64_t DY = (int64_t)Y * BOX_BRUSH_ONE + BOX_BRUSH_ONE / 2 - P->Space.F;
    const int64_t U  = BoxBrushRound(Ink->IA * DX + Ink->IC * DY);
    const int64_t V  = BoxBrushRound(Ink->IB * DX + Ink->ID * DY);
    if (P->Kind == BOX_BRUSH_PAINT_PICTURE) return BoxBrushSample(P, U, V);
    if (P->Kind == BOX_BRUSH_PAINT_LINEAR) return BoxBrushAlong(P, (uint32_t)BoxBrushLeast(BoxBrushMost(U, 0), BOX_BRUSH_GRADIENT));
    if (BoxBrushSize(U) > 4 * BOX_BRUSH_GRADIENT || BoxBrushSize(V) > 4 * BOX_BRUSH_GRADIENT)
        return BoxBrushAlong(P, BOX_BRUSH_GRADIENT);
    /* Its way's end and past it the last colour, no root asked: the root of
     * Far reaches GRADIENT exactly when Far reaches its square. */
    const uint64_t Far = (uint64_t)(U * U + V * V);
    if (Far >= (uint64_t)BOX_BRUSH_GRADIENT * BOX_BRUSH_GRADIENT) return BoxBrushAlong(P, BOX_BRUSH_GRADIENT);
    return BoxBrushAlong(P, BoxBrushRoot((uint32_t)Far));
}

/* ---------------------------------------------------------------------------
 * Filling
 * ------------------------------------------------------------------------- */

/* ∫ from 0 to Z of min(max(t, 0), 1) dt, twice over, Z in places — in
 * 1/(ONE·ONE) of a pixel. */
static inline int64_t BoxBrushRamp(int64_t Z)
{
    if (Z <= 0) return 0;
    if (Z >= BOX_BRUSH_ONE) return 2 * BOX_BRUSH_ONE * Z - BOX_BRUSH_ONE * BOX_BRUSH_ONE;
    return Z * Z;
}

/* How much of the row's height Dy the part of an edge from Left to Right
 * across (Wide apart) covers of the pixels left of Boundary, from the edge
 * rightwards: Dy·ONE when the edge lies wholly left of it. */
static inline int64_t BoxBrushCover(int64_t Left, int64_t Right, int64_t Wide, int64_t Dy, int64_t Boundary)
{
    if (Wide == 0) return Dy * BoxBrushLeast(BoxBrushMost(Boundary - Left, 0), BOX_BRUSH_ONE);
    return Dy * (BoxBrushRamp(Boundary - Left) - BoxBrushRamp(Boundary - Right)) / (2 * Wide);
}

/* What edge E adds to row Y's cells: what it adds to every column left of
 * First into Row[0], to column C (First ≤ C < End) into Row[1 + C - First] —
 * as much as the cell's coverage differs from the one left of it. */
static inline void BoxBrushRowEdge(int32_t *Row, int32_t First, int32_t End, const BoxBrushEdge *E, int32_t Y)
{
    int64_t XT = E->X0, YT = E->Y0, XB = E->X1, YB = E->Y1, Dir = 1;
    if (YT > YB) {
        XT  = E->X1;
        YT  = E->Y1;
        XB  = E->X0;
        YB  = E->Y0;
        Dir = -1;
    }
    const int64_t Top = (int64_t)Y * BOX_BRUSH_ONE, From = BoxBrushMost(YT, Top), To = BoxBrushLeast(YB, Top + BOX_BRUSH_ONE);
    if (From >= To) return;
    /* Where the edge crosses a height, weighed from both ends alike: the edge
     * turned upside down crosses at the same places. */
    const int64_t Span   = YB - YT;
    const int64_t XFrom  = BoxBrushRoundDiv(XT * (YB - From) + XB * (From - YT), Span);
    const int64_t XTo    = BoxBrushRoundDiv(XT * (YB - To) + XB * (To - YT), Span);
    const int64_t Left   = BoxBrushLeast(XFrom, XTo), Right = BoxBrushMost(XFrom, XTo), Wide = Right - Left;
    const int64_t Dy     = (To - From) * Dir;
    const int64_t Whole  = BoxBrushPixelOf(Right) + 1;   /* the first column the edge lies wholly left of */
    int64_t       Column = BoxBrushPixelOf(Left), Before = 0;
    if (Column < First) {
        Before = BoxBrushCover(Left, Right, Wide, Dy, (int64_t)First * BOX_BRUSH_ONE);
        Row[0] += (int32_t)Before;
        Column = First;
    }
    for (; Column <= Whole && Column < End; Column++) {
        const int64_t Covered = BoxBrushCover(Left, Right, Wide, Dy, (Column + 1) * BOX_BRUSH_ONE);
        Row[1 + Column - First] += (int32_t)(Covered - Before);
        Before = Covered;
    }
}

/* The pixels of the canvas's clip the closed outline may cover: columns
 * Left to End, rows Top to Bottom. Empty when it covers none. */
static inline BoxBrushRect BoxBrushReach(const BoxBrushCanvas *K, const BoxBrushOutline *O)
{
    const BoxBrushRect Within = BoxBrushWithin(K);
    BoxBrushRect       Held   = { 0, 0, 0, 0 };
    if (O->Count == 0) return Held;
    Held.X      = (int32_t)BoxBrushPixelOf(O->Left);
    Held.Y      = (int32_t)BoxBrushPixelOf(O->Top);
    Held.Width  = (int32_t)(BoxBrushPixelOf(O->Right) + 1 - Held.X);
    Held.Height = (int32_t)(BoxBrushPixelOf((int64_t)O->Bottom + BOX_BRUSH_ONE - 1) - Held.Y);
    return BoxBrushMeet(Held, Within);
}

/* Row Y's cells, from column Left to End, laid with every edge of the
 * outline: pixel X covers |Row[0] + Row[1] + … + Row[1 + X - Left]| of
 * ONE·ONE, no more than once over. */
static inline void BoxBrushRowCells(const BoxBrushCanvas *K, const BoxBrushOutline *O, int32_t Y, int32_t Left,
                                    int32_t End)
{
    for (int32_t C = 0; C <= End - Left; C++) K->Row[C] = 0;
    for (uint32_t I = 0; I < O->Count; I++) BoxBrushRowEdge(K->Row, Left, End, &O->Edges[I], Y);
}

/* The running sum of a row's cells as a share of 255 of the pixel. */
static inline uint32_t BoxBrushCoverage(int64_t Run)
{
    const int64_t Covered = BoxBrushLeast(BoxBrushSize(Run), (int64_t)BOX_BRUSH_ONE * BOX_BRUSH_ONE);
    return (uint32_t)((Covered * BOX_BRUSH_FULL + 32768) >> 16);
}

/* The outline (closed first) filled with the paint at Opacity of 255, within
 * the canvas's clip. 0, and nothing drawn, when the outline spilled or the
 * paint does not fit. */
static inline int BoxBrushFill(const BoxBrushCanvas *K, BoxBrushOutline *O, const BoxBrushPaint *P, uint32_t Opacity)
{
    BoxBrushInk Ink;
    BoxBrushClose(O);
    if (O->Spilled || !BoxBrushInkOf(P, &Ink)) return 0;
    if (Opacity == 0) return 1;

    const BoxBrushRect Held  = BoxBrushReach(K, O);
    const int32_t      Left  = Held.X, End = Held.X + Held.Width;
    const int          Plain = P->Kind == BOX_BRUSH_PAINT_SOLID && (P->Color >> 24) == BOX_BRUSH_FULL &&
                               Opacity >= BOX_BRUSH_FULL;
    for (int32_t Y = Held.Y; Y < Held.Y + Held.Height; Y++) {
        BoxBrushRowCells(K, O, Y, Left, End);
        uint32_t *Line = K->Pixels + (size_t)Y * K->Stride;
        int64_t   Run  = K->Row[0];
        for (int32_t X = Left; X < End; X++) {
            Run += K->Row[1 + X - Left];
            const uint32_t Coverage = BoxBrushCoverage(Run);
            if (Coverage == 0) continue;
            if (Plain && Coverage == BOX_BRUSH_FULL) {
                Line[X] = P->Color & 0x00FFFFFFu;
                continue;
            }
            const uint32_t Color = BoxBrushInkAt(&Ink, X, Y);
            const uint32_t Share = BoxBrushShare(Coverage, Color >> 24, Opacity);
            if (Share) Line[X] = BoxColorOver(Line[X], Color, Share);
        }
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Text
 * ------------------------------------------------------------------------- */

/* Text — Bytes of UTF-8 — in the imprint's size Size, its first cell's
 * top-left corner at pixel X, Y, in Color (0xAARRGGBB) at Opacity: what it
 * covers of each pixel laid as a shape's coverage is. A character the imprint
 * lacks is its empty box. The pixels it took across; none drawn, and 0, for a
 * size the imprint has not. */
static inline int32_t BoxBrushText(const BoxBrushCanvas *K, const BoxImprint *Imprint, uint32_t Size, int32_t X,
                                   int32_t Y, const char *Text, size_t Bytes, uint32_t Color, uint32_t Opacity)
{
    if (Size >= Imprint->SizeCount) return 0;
    const BoxImprintSize *Cell   = &Imprint->Sizes[Size];
    const BoxBrushRect    Within = BoxBrushWithin(K);
    int64_t               At     = X;
    size_t                Read   = 0;
    while (Read < Bytes) {
        uint32_t Character;
        Read += BoxUtf8Next(Text + Read, Bytes - Read, &Character);
        const uint32_t Glyph = BoxImprintFind(Imprint, Character);
        for (uint32_t GY = 0; GY < Cell->Height; GY++) {
            const int64_t PY = (int64_t)Y + GY;
            if (PY < Within.Y || PY >= (int64_t)Within.Y + Within.Height) continue;
            uint32_t *Line = K->Pixels + (size_t)PY * K->Stride;
            for (uint32_t GX = 0; GX < Cell->Width; GX++) {
                const int64_t PX = At + GX;
                if (PX < Within.X || PX >= (int64_t)Within.X + Within.Width) continue;
                const uint32_t Inked = BoxImprintShade(Imprint, Size, Glyph, GX, GY);
                const uint32_t Share =
                    Inked ? BoxBrushShare(Inked * BOX_BRUSH_FULL / BOXOS_IMPRINT_INKED, Color >> 24, Opacity) : 0u;
                if (Share) Line[PX] = BoxColorOver(Line[PX], Color, Share);
            }
        }
        At += Cell->Width;
    }
    return (int32_t)(At - X);
}

#endif /* BOXOS_BRUSH_H */
