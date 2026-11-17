/*
 * boxos_shape.h — shapes: the outlines a picture is made of, and how much of
 * each pixel they cover. One text for every hand that draws: the brush
 * (boxos_brush.h — Gangway and the scenes), Typecase (every glyph, and the
 * imprint pressed from them) and Painter's engine (painter/core) — so a shape
 * covers the very same share of a pixel in the loader, in the system and on
 * the host. The scene stand (tools/scene_stand.c) holds it to its areas.
 *
 * Whole numbers only: the loader runs with no SSE, and numbers worked the same
 * way everywhere make the same picture everywhere. No memory is asked for: the
 * caller hands in every room a shape is built and swept in.
 *
 * A shape is contours of straight edges — the curves and the round shapes
 * broken into edges that stray less than a sixteenth of a pixel where they
 * land — and a pixel's share of it is worked out from its edges exactly: the
 * signed area each adds to the cells of a row. An edge is as smooth at any
 * slope, size and turn. Which pixels the shape holds is its rule: NONZERO,
 * where its contours wind round a pixel at all — a contour wound the other way
 * cuts a hole, contours wound alike join — or EVENODD, where they wind round
 * it an odd number of times, whichever way they run. Where only the edges of
 * two contours share a pixel, their shares add, up to the whole of it.
 *
 * Places are in 1/BOX_SHAPE_ONE of a pixel, a matrix's numbers in
 * 1/BOX_SHAPE_UNIT, angles in 1/BOX_SHAPE_TURN of a turn. No place of a
 * shape's own lies further than BOX_SHAPE_OWN_MOST from its 0,0, none lands
 * further than BOX_SHAPE_REACH pixels from the canvas's corner, and no matrix
 * stretches by more than BOX_SHAPE_STRETCH_MOST: a shape that would is
 * refused, and says so — never wrapped round into a wrong picture.
 *
 * The includer supplies uint8_t, uint32_t, int32_t, uint64_t, int64_t and
 * size_t.
 */
#ifndef BOXOS_SHAPE_H
#define BOXOS_SHAPE_H

#define BOX_SHAPE_ONE          256       /* a pixel, in places */
#define BOX_SHAPE_UNIT         65536     /* 1, in a matrix's numbers */
#define BOX_SHAPE_TURN         65536u    /* a whole turn */
#define BOX_SHAPE_REACH        32768     /* pixels: the furthest any place lies from the canvas's corner */
#define BOX_SHAPE_STRETCH_MOST 256       /* the most a matrix stretches a length by */
#define BOX_SHAPE_OWN_MOST     (1 << 26) /* places: the furthest a shape's own place or size reaches */
#define BOX_SHAPE_FULL         255u      /* a share that is whole */

/* The furthest a contour strays from the curve it stands for, in places on
 * the canvas: a sixteenth of a pixel, fine enough for the curves of a glyph. */
#define BOX_SHAPE_FLAT (BOX_SHAPE_ONE / 16)

/* The most edges a round shape or a curve is broken into. */
#define BOX_SHAPE_STEPS_MOST 2048u

/* The most edges a hand that grows its room gives one shape — 64 MiB of
 * them: a shape that wants more is refused, so no shape asks for memory or
 * time past a bound. */
#define BOX_SHAPE_EDGES_MOST (1u << 22)

/* Which pixels a shape holds. */
#define BOX_SHAPE_NONZERO 0u   /* where its contours wind round a pixel at all */
#define BOX_SHAPE_EVENODD 1u   /* where they wind round it an odd number of times */

/* ---------------------------------------------------------------------------
 * Numbers
 * ------------------------------------------------------------------------- */

/* The whole number nearest below the square root of Value, by tables: the
 * way of a hand with no SSE (the loader). */
static inline uint32_t BoxShapeRootByTable(uint32_t Value)
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
    uint32_t Root = (BoxShapeRootByTable(Value >> Shift) << (Shift / 2u)) + (1u << (Shift / 2u - 1u));
    Root = (Root + Value / Root) / 2u;
    if ((uint64_t)Root * Root > Value) Root--;
    else if ((uint64_t)(Root + 1u) * (Root + 1u) <= Value) Root++;
    return Root;
}

/* The whole number nearest below the square root of Value, a bit at a
 * time: the way of a hand with no SSE. */
static inline uint64_t BoxShapeRootByBits(uint64_t Value)
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

#if defined(__x86_64__) && defined(__SSE2__)
/* The whole number nearest below the square root of Value by the
 * processor's own square root: a double's root lies within one of the true
 * one, and the steps after make it exact — the very number the ways above
 * give, as the draw stand holds, in a few cycles instead of a loop. */
static inline uint64_t BoxShapeRootBySse2(uint64_t Value)
{
    const double Seen = (double)Value;
    double       Root;
    __asm__("sqrtsd %1, %0" : "=x"(Root) : "x"(Seen));
    uint64_t Whole = (uint64_t)Root;
    if (Whole > 0xFFFFFFFFull) Whole = 0xFFFFFFFFull;
    while (Whole * Whole > Value) Whole--;
    while (Whole < 0xFFFFFFFFull && (Whole + 1u) * (Whole + 1u) <= Value) Whole++;
    return Whole;
}
#endif

/* The whole number nearest below the square root of Value — by the
 * processor's square root where the hand has SSE2, by tables where it has
 * not: one number either way. */
static inline uint32_t BoxShapeRoot(uint32_t Value)
{
#if defined(__x86_64__) && defined(__SSE2__)
    return (uint32_t)BoxShapeRootBySse2(Value);
#else
    return BoxShapeRootByTable(Value);
#endif
}

static inline uint64_t BoxShapeRoot64(uint64_t Value)
{
#if defined(__x86_64__) && defined(__SSE2__)
    return BoxShapeRootBySse2(Value);
#else
    return BoxShapeRootByBits(Value);
#endif
}

/* The sine of Turn, in 1/BOX_SHAPE_UNIT: a quarter of the wave kept, the rest
 * its mirror, so every quarter turn is exactly 0 or 1 and the wave is the same
 * on both sides of each. */
static inline int32_t BoxShapeSine(uint32_t Turn)
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

static inline int32_t BoxShapeCosine(uint32_t Turn)
{
    return BoxShapeSine(Turn + BOX_SHAPE_TURN / 4u);
}

/* The turn from the x axis round to X, Y, toward y: 0 to TURN, a quarter turn
 * at +y; 0 for 0, 0. An eighth of the turns kept — the turn whose tangent is
 * k/256, in 1/2^24 of a turn — the rest its mirrors, so the turns of X, Y and
 * of Y, X add up to a quarter exactly, and a mirrored place turns the mirrored
 * way. */
static inline uint32_t BoxShapeTurnOf(int64_t X, int64_t Y)
{
    static const int32_t Eighth[257] = {
              0,   10430,   20860,   31290,   41718,   52145,   62571,   72994,
          83416,   93835,  104251,  114664,  125073,  135479,  145880,  156277,
         166669,  177056,  187438,  197815,  208185,  218549,  228906,  239256,
         249600,  259935,  270263,  280583,  290894,  301197,  311491,  321775,
         332050,  342315,  352570,  362814,  373047,  383270,  393481,  403681,
         413869,  424044,  434208,  444358,  454496,  464620,  474731,  484829,
         494912,  504981,  515035,  525075,  535100,  545109,  555103,  565081,
         575043,  584989,  594918,  604831,  614727,  624606,  634467,  644311,
         654136,  663944,  673734,  683505,  693257,  702990,  712705,  722400,
         732076,  741732,  751368,  760984,  770579,  780155,  789709,  799243,
         808756,  818248,  827718,  837168,  846595,  856001,  865384,  874746,
         884085,  893402,  902696,  911968,  921217,  930443,  939645,  948825,
         957981,  967114,  976223,  985308,  994370, 1003407, 1012421, 1021410,
        1030375, 1039316, 1048232, 1057123, 1065990, 1074832, 1083649, 1092442,
        1101209, 1109951, 1118668, 1127359, 1136026, 1144667, 1153282, 1161872,
        1170436, 1178975, 1187488, 1195975, 1204436, 1212871, 1221280, 1229664,
        1238021, 1246352, 1254658, 1262937, 1271189, 1279416, 1287616, 1295790,
        1303938, 1312059, 1320154, 1328223, 1336265, 1344281, 1352271, 1360234,
        1368170, 1376081, 1383964, 1391822, 1399652, 1407457, 1415234, 1422986,
        1430711, 1438409, 1446081, 1453727, 1461346, 1468939, 1476505, 1484045,
        1491559, 1499046, 1506507, 1513942, 1521350, 1528733, 1536089, 1543419,
        1550722, 1558000, 1565251, 1572477, 1579676, 1586849, 1593997, 1601118,
        1608214, 1615284, 1622328, 1629346, 1636338, 1643305, 1650246, 1657162,
        1664052, 1670917, 1677757, 1684570, 1691359, 1698123, 1704861, 1711574,
        1718262, 1724925, 1731563, 1738176, 1744764, 1751327, 1757866, 1764380,
        1770869, 1777334, 1783774, 1790190, 1796582, 1802949, 1809292, 1815611,
        1821906, 1828177, 1834423, 1840646, 1846846, 1853021, 1859173, 1865301,
        1871405, 1877486, 1883544, 1889578, 1895590, 1901578, 1907542, 1913484,
        1919403, 1925299, 1931173, 1937023, 1942851, 1948656, 1954439, 1960199,
        1965938, 1971653, 1977347, 1983018, 1988668, 1994295, 1999901, 2005485,
        2011047, 2016588, 2022107, 2027604, 2033080, 2038535, 2043968, 2049381,
        2054772, 2060142, 2065491, 2070820, 2076127, 2081414, 2086681, 2091927,
        2097152
    };
    uint64_t Small = (uint64_t)(X < 0 ? -X : X), Large = (uint64_t)(Y < 0 ? -Y : Y);
    const int Steep = Large > Small;
    if (!Steep) {
        const uint64_t Swap = Small;
        Small = Large;
        Large = Swap;
    }
    if (Large == 0) return 0;
    while (Large >= (1ull << 46)) {
        Small >>= 1;
        Large >>= 1;
    }
    const uint64_t Ratio = (Small << 16) / Large;   /* the tangent, 0 to 65536 */
    const uint32_t Step = (uint32_t)(Ratio >> 8), Part = (uint32_t)(Ratio & 255u);
    int64_t        Value = Eighth[Step];
    if (Part) Value += ((int64_t)(Eighth[Step + 1u] - Eighth[Step]) * Part) >> 8;
    uint32_t Turn = (uint32_t)((Value + 128) >> 8);
    if (Steep) Turn = BOX_SHAPE_TURN / 4u - Turn;
    if (X < 0) Turn = BOX_SHAPE_TURN / 2u - Turn;
    if (Y < 0) Turn = BOX_SHAPE_TURN - Turn;
    return Turn & (BOX_SHAPE_TURN - 1u);
}

static inline int64_t BoxShapeLeast(int64_t A, int64_t B) { return A < B ? A : B; }
static inline int64_t BoxShapeMost(int64_t A, int64_t B) { return A > B ? A : B; }
static inline int64_t BoxShapeSize(int64_t A) { return A < 0 ? -A : A; }

/* Value / UNIT to the nearest whole, halves away from 0 — the same on both
 * sides of 0, so a shape and its mirror land on mirrored places. */
static inline int64_t BoxShapeRound(int64_t Value)
{
    return Value >= 0 ? (Value + BOX_SHAPE_UNIT / 2) / BOX_SHAPE_UNIT : -((-Value + BOX_SHAPE_UNIT / 2) / BOX_SHAPE_UNIT);
}

/* Num / Den, Den above 0, to the nearest whole, halves away from 0. */
static inline int64_t BoxShapeRoundDiv(int64_t Num, int64_t Den)
{
    return Num >= 0 ? (Num + Den / 2) / Den : -((-Num + Den / 2) / Den);
}

/* The pixel a place lies in: Place / ONE, rounded down. */
static inline int64_t BoxShapePixelOf(int64_t Place)
{
    return Place >= 0 ? Place / BOX_SHAPE_ONE : -((-Place + BOX_SHAPE_ONE - 1) / BOX_SHAPE_ONE);
}

/* A number kept to int32's room: one that would not fit is past what a shape
 * is drawn with, and is refused as one. */
static inline int32_t BoxShapeNarrow(int64_t Value)
{
    return Value > 2147483647ll ? 2147483647 : Value < -2147483647ll ? -2147483647 : (int32_t)Value;
}

/* How far apart, in places, two places are. */
static inline int64_t BoxShapeApart(int64_t X, int64_t Y)
{
    return (int64_t)BoxShapeRoot64((uint64_t)(X * X + Y * Y));
}

/* ---------------------------------------------------------------------------
 * Rectangles, matrices
 * ------------------------------------------------------------------------- */

/* Whole pixels. */
typedef struct BoxRect {
    int32_t X, Y, Width, Height;
} BoxRect;

static inline BoxRect BoxRectMeet(BoxRect A, BoxRect B)
{
    const int64_t Left   = BoxShapeMost(A.X, B.X), Top = BoxShapeMost(A.Y, B.Y);
    const int64_t Right  = BoxShapeLeast((int64_t)A.X + A.Width, (int64_t)B.X + B.Width);
    const int64_t Bottom = BoxShapeLeast((int64_t)A.Y + A.Height, (int64_t)B.Y + B.Height);
    BoxRect       Met    = { 0, 0, 0, 0 };
    if (Right <= Left || Bottom <= Top) return Met;
    Met.X      = (int32_t)Left;
    Met.Y      = (int32_t)Top;
    Met.Width  = (int32_t)(Right - Left);
    Met.Height = (int32_t)(Bottom - Top);
    return Met;
}

/* The least rectangle holding both; an empty one holds nothing. */
static inline BoxRect BoxRectJoin(BoxRect A, BoxRect B)
{
    if (A.Width <= 0 || A.Height <= 0) return B;
    if (B.Width <= 0 || B.Height <= 0) return A;
    const int64_t Left   = BoxShapeLeast(A.X, B.X), Top = BoxShapeLeast(A.Y, B.Y);
    const int64_t Right  = BoxShapeMost((int64_t)A.X + A.Width, (int64_t)B.X + B.Width);
    const int64_t Bottom = BoxShapeMost((int64_t)A.Y + A.Height, (int64_t)B.Y + B.Height);
    BoxRect       Joined = { (int32_t)Left, (int32_t)Top, (int32_t)(Right - Left), (int32_t)(Bottom - Top) };
    return Joined;
}

/*
 * Where a place of a shape's or a paint's own space lands on the canvas —
 * SVG's matrix(a b c d e f):
 *     x' = (A·x + C·y) / UNIT + E        y' = (B·x + D·y) / UNIT + F
 * A to D in 1/UNIT, E and F places.
 */
typedef struct BoxMatrix {
    int32_t A, B, C, D, E, F;
} BoxMatrix;

/* Turned by Turn, stretched by Stretch (1/UNIT), its own 0,0 put at X, Y. */
static inline BoxMatrix BoxMatrixPlaced(int32_t X, int32_t Y, int32_t Stretch, uint32_t Turn)
{
    const int64_t Cos = BoxShapeCosine(Turn), Sin = BoxShapeSine(Turn);
    BoxMatrix     M;
    M.A = BoxShapeNarrow(BoxShapeRound(Cos * Stretch));
    M.B = BoxShapeNarrow(BoxShapeRound(Sin * Stretch));
    M.C = -M.B;
    M.D = M.A;
    M.E = X;
    M.F = Y;
    return M;
}

/* M, and then N. */
static inline BoxMatrix BoxMatrixThen(BoxMatrix M, BoxMatrix N)
{
    BoxMatrix R;
    R.A = BoxShapeNarrow(BoxShapeRound((int64_t)N.A * M.A + (int64_t)N.C * M.B));
    R.B = BoxShapeNarrow(BoxShapeRound((int64_t)N.B * M.A + (int64_t)N.D * M.B));
    R.C = BoxShapeNarrow(BoxShapeRound((int64_t)N.A * M.C + (int64_t)N.C * M.D));
    R.D = BoxShapeNarrow(BoxShapeRound((int64_t)N.B * M.C + (int64_t)N.D * M.D));
    R.E = BoxShapeNarrow(BoxShapeRound((int64_t)N.A * M.E + (int64_t)N.C * M.F) + N.E);
    R.F = BoxShapeNarrow(BoxShapeRound((int64_t)N.B * M.E + (int64_t)N.D * M.F) + N.F);
    return R;
}

/* Whether a shape is drawn with M: no number stretches past STRETCH_MOST and
 * its 0,0 lies within REACH. */
static inline int BoxMatrixFits(const BoxMatrix *M)
{
    const int64_t Most  = (int64_t)BOX_SHAPE_STRETCH_MOST * BOX_SHAPE_UNIT;
    const int64_t Reach = (int64_t)BOX_SHAPE_REACH * BOX_SHAPE_ONE;
    return BoxShapeSize(M->A) <= Most && BoxShapeSize(M->B) <= Most && BoxShapeSize(M->C) <= Most &&
           BoxShapeSize(M->D) <= Most && BoxShapeSize(M->E) <= Reach && BoxShapeSize(M->F) <= Reach;
}

/* The most M stretches a length by, in 1/UNIT — the longer of its two axes.
 * M fits. */
static inline int64_t BoxMatrixStretch(const BoxMatrix *M)
{
    const uint64_t X = (uint64_t)((int64_t)M->A * M->A + (int64_t)M->B * M->B);
    const uint64_t Y = (uint64_t)((int64_t)M->C * M->C + (int64_t)M->D * M->D);
    return (int64_t)BoxShapeRoot64(X > Y ? X : Y);
}

/* ---------------------------------------------------------------------------
 * Building a shape
 * ------------------------------------------------------------------------- */

/* An edge on the canvas, in places, from its first end to its second — the
 * way it runs is the way its contour winds. Never level: a level edge covers
 * nothing. */
typedef struct BoxShapeEdge {
    int32_t X0, Y0, X1, Y1;
} BoxShapeEdge;

/*
 * A shape being built: the edges its contours have made so far, into the
 * caller's room, every place drawn through Matrix onto the canvas. Spilled: a
 * place lay past OWN_MOST or landed past REACH, the matrix does not fit, or an
 * edge was drawn with no contour begun — it is refused. Short: its edges found
 * no room past Room — it is refused too, and Wanted says the room it needs.
 */
typedef struct BoxShape {
    BoxShapeEdge *Edges;
    uint32_t      Room;
    uint32_t      Count;                     /* edges kept */
    uint32_t      Wanted;                    /* edges made, kept or not */
    uint32_t      Rule;                      /* NONZERO or EVENODD */
    BoxMatrix     Matrix;
    int64_t       Stretch;                   /* the most Matrix stretches a length by, 1/UNIT */
    int32_t       Left, Top, Right, Bottom;  /* the places every edge kept lies within */
    int32_t       StartX, StartY;            /* the contour's first place, on the canvas */
    int32_t       AtX, AtY;                  /* the pen */
    int           Open;                      /* a contour is begun and not closed */
    int           Spilled;
    int           Short;
} BoxShape;

static inline void BoxShapeBegin(BoxShape *S, BoxShapeEdge *Edges, uint32_t Room, const BoxMatrix *Matrix)
{
    S->Edges   = Edges;
    S->Room    = Room;
    S->Count   = 0;
    S->Wanted  = 0;
    S->Rule    = BOX_SHAPE_NONZERO;
    S->Matrix  = *Matrix;
    S->Left    = S->Top = 2147483647;
    S->Right   = S->Bottom = -2147483647;
    S->StartX  = S->StartY = S->AtX = S->AtY = 0;
    S->Open    = 0;
    S->Short   = 0;
    S->Spilled = !BoxMatrixFits(Matrix);
    S->Stretch = S->Spilled ? 0 : BoxMatrixStretch(Matrix);
}

/* Whether every one of Count places or sizes of a shape's own lies within
 * OWN_MOST; the shape spilled when one does not. */
static inline int BoxShapeOwn(BoxShape *S, const int32_t *Values, uint32_t Count)
{
    for (uint32_t I = 0; I < Count; I++)
        if (BoxShapeSize(Values[I]) > BOX_SHAPE_OWN_MOST) S->Spilled = 1;
    return !S->Spilled;
}

/* X, Y of the shape's own space, on the canvas: 0 when it lies past OWN_MOST
 * or lands past REACH. */
static inline int BoxShapeLand(BoxShape *S, int32_t X, int32_t Y, int32_t *CanvasX, int32_t *CanvasY)
{
    const int32_t Own[2] = { X, Y };
    if (!BoxShapeOwn(S, Own, 2)) return 0;
    const BoxMatrix *M     = &S->Matrix;
    const int64_t    Reach = (int64_t)BOX_SHAPE_REACH * BOX_SHAPE_ONE;
    const int64_t    MX    = BoxShapeRound((int64_t)M->A * X + (int64_t)M->C * Y) + M->E;
    const int64_t    MY    = BoxShapeRound((int64_t)M->B * X + (int64_t)M->D * Y) + M->F;
    if (BoxShapeSize(MX) > Reach || BoxShapeSize(MY) > Reach) {
        S->Spilled = 1;
        return 0;
    }
    *CanvasX = (int32_t)MX;
    *CanvasY = (int32_t)MY;
    return 1;
}

/* An edge from the pen to X, Y on the canvas. */
static inline void BoxShapeEdgeTo(BoxShape *S, int32_t X, int32_t Y)
{
    if (Y != S->AtY) {
        S->Wanted++;
        if (S->Count == S->Room) {
            S->Short = 1;
        } else {
            BoxShapeEdge *E = &S->Edges[S->Count++];
            E->X0           = S->AtX;
            E->Y0           = S->AtY;
            E->X1           = X;
            E->Y1           = Y;
            S->Left         = (int32_t)BoxShapeLeast(S->Left, BoxShapeLeast(X, S->AtX));
            S->Right        = (int32_t)BoxShapeMost(S->Right, BoxShapeMost(X, S->AtX));
            S->Top          = (int32_t)BoxShapeLeast(S->Top, BoxShapeLeast(Y, S->AtY));
            S->Bottom       = (int32_t)BoxShapeMost(S->Bottom, BoxShapeMost(Y, S->AtY));
        }
    }
    S->AtX = X;
    S->AtY = Y;
}

static inline void BoxShapeClose(BoxShape *S)
{
    if (!S->Open || S->Spilled) return;
    BoxShapeEdgeTo(S, S->StartX, S->StartY);
    S->Open = 0;
}

/* A new contour begun at X, Y; the one open closed first. */
static inline void BoxShapeMoveTo(BoxShape *S, int32_t X, int32_t Y)
{
    int32_t CX, CY;
    BoxShapeClose(S);
    if (S->Spilled || !BoxShapeLand(S, X, Y, &CX, &CY)) return;
    S->StartX = S->AtX = CX;
    S->StartY = S->AtY = CY;
    S->Open            = 1;
}

/* The pen, and the contour's place, before an edge is drawn: none begun
 * spills the shape. */
static inline int BoxShapePen(BoxShape *S)
{
    if (S->Spilled) return 0;
    if (!S->Open) {
        S->Spilled = 1;
        return 0;
    }
    return 1;
}

static inline void BoxShapeLineTo(BoxShape *S, int32_t X, int32_t Y)
{
    int32_t CX, CY;
    if (BoxShapePen(S) && BoxShapeLand(S, X, Y, &CX, &CY)) BoxShapeEdgeTo(S, CX, CY);
}

/* How many edges a curve whose control points bend Bend places away from
 * straight is broken into, the bend times Factor quarters of the stray an
 * edge may leave (a quadratic 1, a cubic 3). */
static inline uint32_t BoxShapeCurveSteps(int64_t Bend, uint32_t Factor)
{
    const uint64_t Steps = BoxShapeRoot64((uint64_t)Bend * Factor / (4u * BOX_SHAPE_FLAT)) + 1u;
    return Steps > BOX_SHAPE_STEPS_MOST ? BOX_SHAPE_STEPS_MOST : (uint32_t)Steps;
}

static inline int64_t BoxShapeLerp(int64_t A, int64_t B, int64_t T)
{
    return A + BoxShapeRound((B - A) * T);
}

/* A quadratic curve from the pen through the pull of CX, CY to X, Y. */
static inline void BoxShapeQuadTo(BoxShape *S, int32_t CX, int32_t CY, int32_t X, int32_t Y)
{
    int32_t PX, PY, EX, EY;
    if (!BoxShapePen(S) || !BoxShapeLand(S, CX, CY, &PX, &PY) || !BoxShapeLand(S, X, Y, &EX, &EY)) return;
    const int64_t  SX = S->AtX, SY = S->AtY;
    const uint32_t N  = BoxShapeCurveSteps(BoxShapeApart(SX - 2 * PX + EX, SY - 2 * PY + EY), 1u);
    for (uint32_t I = 1; I < N; I++) {
        const int64_t T = (int64_t)I * BOX_SHAPE_UNIT / N;
        BoxShapeEdgeTo(S, (int32_t)BoxShapeLerp(BoxShapeLerp(SX, PX, T), BoxShapeLerp(PX, EX, T), T),
                       (int32_t)BoxShapeLerp(BoxShapeLerp(SY, PY, T), BoxShapeLerp(PY, EY, T), T));
    }
    BoxShapeEdgeTo(S, EX, EY);
}

/* A cubic curve from the pen, pulled by C1 and C2, to X, Y. */
static inline void BoxShapeCubicTo(BoxShape *S, int32_t C1X, int32_t C1Y, int32_t C2X, int32_t C2Y, int32_t X,
                                   int32_t Y)
{
    int32_t P1X, P1Y, P2X, P2Y, EX, EY;
    if (!BoxShapePen(S) || !BoxShapeLand(S, C1X, C1Y, &P1X, &P1Y) || !BoxShapeLand(S, C2X, C2Y, &P2X, &P2Y) ||
        !BoxShapeLand(S, X, Y, &EX, &EY))
        return;
    const int64_t  SX   = S->AtX, SY = S->AtY;
    const int64_t  Bend = BoxShapeMost(BoxShapeApart(SX - 2 * P1X + P2X, SY - 2 * P1Y + P2Y),
                                       BoxShapeApart(P1X - 2 * P2X + EX, P1Y - 2 * P2Y + EY));
    const uint32_t N    = BoxShapeCurveSteps(Bend, 3u);
    for (uint32_t I = 1; I < N; I++) {
        const int64_t T   = (int64_t)I * BOX_SHAPE_UNIT / N;
        const int64_t AX  = BoxShapeLerp(SX, P1X, T), AY = BoxShapeLerp(SY, P1Y, T);
        const int64_t BX  = BoxShapeLerp(P1X, P2X, T), BY = BoxShapeLerp(P1Y, P2Y, T);
        const int64_t CX  = BoxShapeLerp(P2X, EX, T), CY = BoxShapeLerp(P2Y, EY, T);
        const int64_t ABX = BoxShapeLerp(AX, BX, T), ABY = BoxShapeLerp(AY, BY, T);
        const int64_t BCX = BoxShapeLerp(BX, CX, T), BCY = BoxShapeLerp(BY, CY, T);
        BoxShapeEdgeTo(S, (int32_t)BoxShapeLerp(ABX, BCX, T), (int32_t)BoxShapeLerp(ABY, BCY, T));
    }
    BoxShapeEdgeTo(S, EX, EY);
}

/* How many edges a round shape of Radius places on the canvas is broken into:
 * each strays at most FLAT from the curve, a multiple of eight so the shape
 * is its own mirror across both axes and both diagonals, eight at the
 * fewest. */
static inline uint32_t BoxShapeRoundSteps(int64_t Radius)
{
    /* A turn of n edges strays Radius·(2π/n)²/8: n = 2π·√(Radius / 8·FLAT). */
    const uint64_t Root  = BoxShapeRoot64((uint64_t)BoxShapeMost(Radius, 0) * 65536u / (8u * BOX_SHAPE_FLAT));
    uint64_t       Steps = (Root * 1608u + 65535u) >> 16;   /* 1608/256 is 2π */
    Steps                = (Steps + 7u) & ~7ull;
    return Steps < 8u ? 8u : Steps > BOX_SHAPE_STEPS_MOST ? BOX_SHAPE_STEPS_MOST : (uint32_t)Steps;
}

/* The turn of step Step of Steps along an arc of Span: the steps from either
 * end the same — Step and Steps - Step add up to Span exactly — so an arc is
 * its own mirror about its middle. Steps is above 0. */
static inline uint32_t BoxShapeArcTurn(uint32_t Step, uint32_t Steps, uint32_t Span)
{
    if (2u * Step <= Steps) return (uint32_t)((uint64_t)Step * Span / Steps);
    return Span - (uint32_t)((uint64_t)(Steps - Step) * Span / Steps);
}

/* The length, on the canvas, of Length places of the shape's own space at
 * its longest. */
static inline int64_t BoxShapeOnCanvas(const BoxShape *S, int64_t Length)
{
    return (BoxShapeSize(Length) * S->Stretch) >> 16;
}

/* ---------------------------------------------------------------------------
 * Ready shapes — every one wound the one way, down the right side on the
 * canvas as its own space stands, unless it is a Hole, wound the other.
 * ------------------------------------------------------------------------- */

static inline void BoxShapeAddRect(BoxShape *S, int32_t X, int32_t Y, int32_t Width, int32_t Height, int Hole)
{
    const int32_t Own[4] = { X, Y, Width, Height };
    if (!BoxShapeOwn(S, Own, 4)) return;
    const int32_t Xs[4] = { X, X + Width, X + Width, X };
    const int32_t Ys[4] = { Y, Y, Y + Height, Y + Height };
    for (uint32_t I = 0; I < 4u; I++) {
        const uint32_t K = Hole ? 3u - I : I;
        if (I == 0) BoxShapeMoveTo(S, Xs[K], Ys[K]);
        else        BoxShapeLineTo(S, Xs[K], Ys[K]);
    }
    BoxShapeClose(S);
}

/* An ellipse as it is broken into edges where it lands: Steps corners round
 * CX, CY, each Wide out along its axis — a little outside the ellipse, by
 * √(θ / sin θ), θ the turn of an edge, so the edges cross the curve and cover
 * what it covers, neither less nor more. Each quarter is the same quarter of a
 * circle, turned, so the shape is exactly its own mirror. */
typedef struct BoxShapeOval {
    int32_t  CX, CY;
    int64_t  WideX, WideY;
    uint32_t Steps;
} BoxShapeOval;

static inline BoxShapeOval BoxShapeOvalOf(const BoxShape *S, int32_t CX, int32_t CY, int32_t RX, int32_t RY)
{
    BoxShapeOval  O;
    O.CX    = CX;
    O.CY    = CY;
    O.Steps = BoxShapeRoundSteps(BoxShapeOnCanvas(S, BoxShapeMost(BoxShapeSize(RX), BoxShapeSize(RY))));
    const int64_t Turn = 411775 / O.Steps;   /* θ in 1/UNIT: 2π is 411775 */
    const int64_t Out  = (int64_t)BoxShapeRoot64((uint64_t)(Turn * BOX_SHAPE_UNIT / BoxShapeSine(BOX_SHAPE_TURN / O.Steps)) *
                                                 BOX_SHAPE_UNIT);
    O.WideX = BoxShapeRound(BoxShapeSize(RX) * Out);
    O.WideY = BoxShapeRound(BoxShapeSize(RY) * Out);
    return O;
}

/* Corner K of the oval, from its right going down. */
static inline void BoxShapeOvalPoint(const BoxShapeOval *O, uint32_t K, int32_t *X, int32_t *Y)
{
    const uint32_t Q       = O->Steps / 4u;
    const uint32_t Quarter = K / Q;
    const uint32_t Turn    = BoxShapeArcTurn(K % Q, Q, BOX_SHAPE_TURN / 4u);
    const int64_t  Cos     = BoxShapeCosine(Turn), Sin = BoxShapeSine(Turn);
    /* The quarter's offsets worked unsigned, then given their signs. */
    const int64_t  OX      = BoxShapeRound(O->WideX * (Quarter & 1u ? Sin : Cos));
    const int64_t  OY      = BoxShapeRound(O->WideY * (Quarter & 1u ? Cos : Sin));
    *X                     = (int32_t)(O->CX + (Quarter == 1u || Quarter == 2u ? -OX : OX));
    *Y                     = (int32_t)(O->CY + (Quarter >= 2u ? -OY : OY));
}

static inline void BoxShapeAddEllipse(BoxShape *S, int32_t CX, int32_t CY, int32_t RX, int32_t RY, int Hole)
{
    const int32_t Own[4] = { CX, CY, RX, RY };
    if (!BoxShapeOwn(S, Own, 4)) return;
    const BoxShapeOval O = BoxShapeOvalOf(S, CX, CY, RX, RY);
    for (uint32_t I = 0; I < O.Steps; I++) {
        int32_t X, Y;
        BoxShapeOvalPoint(&O, Hole ? (O.Steps - I) % O.Steps : I, &X, &Y);
        if (I == 0) BoxShapeMoveTo(S, X, Y);
        else        BoxShapeLineTo(S, X, Y);
    }
    BoxShapeClose(S);
}

/*
 * A rectangle whose corners are quarters of circles, each of its own radius,
 * as it is broken into edges where it lands: the contour's corners in its
 * order — 0 top right, 1 bottom right, 2 bottom left, 3 top left, as its own
 * space stands — each Radius round its middle X, Y in Steps edges; a corner of
 * no radius is its one place. Where the two corners of a side would overlap,
 * every radius is shrunk alike until they meet, as CSS shrinks them.
 */
typedef struct BoxShapeRounding {
    int32_t  Radius[4];
    int32_t  X[4], Y[4];
    uint32_t Steps[4];
} BoxShapeRounding;

/* The rounding of a rectangle of Radii — [0] its top left, [1] top right, [2]
 * bottom right, [3] bottom left; 0 when no corner is round. */
static inline int BoxShapeRoundingOf(const BoxShape *S, int32_t X, int32_t Y, int32_t Width, int32_t Height,
                                     const int32_t Radii[4], BoxShapeRounding *R)
{
    int64_t Kept[4];
    for (uint32_t K = 0; K < 4u; K++) Kept[K] = BoxShapeMost(Radii[K], 0);
    /* The side the corners overlap most on, as the least of side / sum. */
    const int64_t Sides[4] = { Width, Height, Width, Height };
    const int64_t Sums[4]  = { Kept[0] + Kept[1], Kept[1] + Kept[2], Kept[2] + Kept[3], Kept[3] + Kept[0] };
    int64_t       Side = 1, Sum = 1;
    for (uint32_t K = 0; K < 4u; K++)
        if (Sums[K] > 0 && Sides[K] * Sum < Side * Sums[K]) {
            Side = Sides[K];
            Sum  = Sums[K];
        }
    if (Side < Sum)
        for (uint32_t K = 0; K < 4u; K++) Kept[K] = Kept[K] * Side / Sum;
    if (Kept[0] == 0 && Kept[1] == 0 && Kept[2] == 0 && Kept[3] == 0) return 0;
    for (uint32_t C = 0; C < 4u; C++) R->Radius[C] = (int32_t)Kept[(C + 1u) % 4u];
    R->X[0] = X + Width - R->Radius[0];
    R->Y[0] = Y + R->Radius[0];
    R->X[1] = X + Width - R->Radius[1];
    R->Y[1] = Y + Height - R->Radius[1];
    R->X[2] = X + R->Radius[2];
    R->Y[2] = Y + Height - R->Radius[2];
    R->X[3] = X + R->Radius[3];
    R->Y[3] = Y + R->Radius[3];
    for (uint32_t C = 0; C < 4u; C++)
        R->Steps[C] = R->Radius[C] > 0 ? BoxShapeRoundSteps(BoxShapeOnCanvas(S, R->Radius[C])) / 4u : 0u;
    return 1;
}

/* Step Step of corner Corner. */
static inline void BoxShapeRoundingPoint(const BoxShapeRounding *R, uint32_t Corner, uint32_t Step, int32_t *X,
                                         int32_t *Y)
{
    *X = R->X[Corner];
    *Y = R->Y[Corner];
    if (R->Radius[Corner] > 0) {
        const uint32_t Turn = (3u + Corner) * (BOX_SHAPE_TURN / 4u) +
                              BoxShapeArcTurn(Step, R->Steps[Corner], BOX_SHAPE_TURN / 4u);
        *X += (int32_t)BoxShapeRound((int64_t)R->Radius[Corner] * BoxShapeCosine(Turn));
        *Y += (int32_t)BoxShapeRound((int64_t)R->Radius[Corner] * BoxShapeSine(Turn));
    }
}

/* A rectangle whose corners are quarters of circles, each of its own radius:
 * Radii[0] the top left, [1] the top right, [2] the bottom right, [3] the
 * bottom left, as the shape's own space stands (BoxShapeRounding); a
 * rectangle of no width or height nothing. */
static inline void BoxShapeAddRounded(BoxShape *S, int32_t X, int32_t Y, int32_t Width, int32_t Height,
                                      const int32_t Radii[4], int Hole)
{
    const int32_t Own[8] = { X, Y, Width, Height, Radii[0], Radii[1], Radii[2], Radii[3] };
    if (!BoxShapeOwn(S, Own, 8) || Width <= 0 || Height <= 0) return;
    BoxShapeRounding R;
    if (!BoxShapeRoundingOf(S, X, Y, Width, Height, Radii, &R)) {
        BoxShapeAddRect(S, X, Y, Width, Height, Hole);
        return;
    }
    int First = 1;
    for (uint32_t I = 0; I < 4u; I++) {
        const uint32_t C = Hole ? 3u - I : I;
        for (uint32_t J = 0; J <= R.Steps[C]; J++) {
            int32_t PX, PY;
            BoxShapeRoundingPoint(&R, C, Hole ? R.Steps[C] - J : J, &PX, &PY);
            if (First) BoxShapeMoveTo(S, PX, PY);
            else       BoxShapeLineTo(S, PX, PY);
            First = 0;
        }
    }
    BoxShapeClose(S);
}

/* ---------------------------------------------------------------------------
 * Coverage
 * ------------------------------------------------------------------------- */

/* ∫ from 0 to Z of min(max(t, 0), 1) dt, twice over, Z in places — in
 * 1/(ONE·ONE) of a pixel. */
static inline int64_t BoxShapeRamp(int64_t Z)
{
    if (Z <= 0) return 0;
    if (Z >= BOX_SHAPE_ONE) return 2 * BOX_SHAPE_ONE * Z - BOX_SHAPE_ONE * BOX_SHAPE_ONE;
    return Z * Z;
}

/* How much of the row's height Dy the part of an edge from Left to Right
 * across (Wide apart) covers of the pixels left of Boundary, from the edge
 * rightwards: Dy·ONE when the edge lies wholly left of it. */
static inline int64_t BoxShapeCover(int64_t Left, int64_t Right, int64_t Wide, int64_t Dy, int64_t Boundary)
{
    if (Wide == 0) return Dy * BoxShapeLeast(BoxShapeMost(Boundary - Left, 0), BOX_SHAPE_ONE);
    return Dy * (BoxShapeRamp(Boundary - Left) - BoxShapeRamp(Boundary - Right)) / (2 * Wide);
}

/* What edge E adds to row Y's cells: what it adds to every column left of
 * First into Cells[0], to column C (First ≤ C < End) into
 * Cells[1 + C - First] — as much as the cell's coverage differs from the one
 * left of it. */
static inline void BoxShapeRowEdge(int32_t *Cells, int32_t First, int32_t End, const BoxShapeEdge *E, int32_t Y)
{
    int64_t XT = E->X0, YT = E->Y0, XB = E->X1, YB = E->Y1, Dir = 1;
    if (YT > YB) {
        XT  = E->X1;
        YT  = E->Y1;
        XB  = E->X0;
        YB  = E->Y0;
        Dir = -1;
    }
    const int64_t Top = (int64_t)Y * BOX_SHAPE_ONE, From = BoxShapeMost(YT, Top), To = BoxShapeLeast(YB, Top + BOX_SHAPE_ONE);
    if (From >= To) return;
    /* Where the edge crosses a height, weighed from both ends alike: the edge
     * turned upside down crosses at the same places. */
    const int64_t Span   = YB - YT;
    const int64_t XFrom  = BoxShapeRoundDiv(XT * (YB - From) + XB * (From - YT), Span);
    const int64_t XTo    = BoxShapeRoundDiv(XT * (YB - To) + XB * (To - YT), Span);
    const int64_t Left   = BoxShapeLeast(XFrom, XTo), Right = BoxShapeMost(XFrom, XTo), Wide = Right - Left;
    const int64_t Dy     = (To - From) * Dir;
    const int64_t Whole  = BoxShapePixelOf(Right) + 1;   /* the first column the edge lies wholly left of */
    int64_t       Column = BoxShapePixelOf(Left), Before = 0;
    if (Column < First) {
        Before = BoxShapeCover(Left, Right, Wide, Dy, (int64_t)First * BOX_SHAPE_ONE);
        Cells[0] += (int32_t)Before;
        Column = First;
    }
    for (; Column <= Whole && Column < End; Column++) {
        const int64_t Covered = BoxShapeCover(Left, Right, Wide, Dy, (Column + 1) * BOX_SHAPE_ONE);
        Cells[1 + Column - First] += (int32_t)(Covered - Before);
        Before = Covered;
    }
}

/* Whether the shape is drawn: neither spilled nor short of room. */
static inline int BoxShapeDrawable(const BoxShape *S)
{
    return !S->Spilled && !S->Short;
}

/* The pixels of Within the closed shape may cover: columns Left to End, rows
 * Top to Bottom. Empty when it covers none. */
static inline BoxRect BoxShapeReach(const BoxShape *S, BoxRect Within)
{
    BoxRect Held = { 0, 0, 0, 0 };
    if (S->Count == 0) return Held;
    Held.X      = (int32_t)BoxShapePixelOf(S->Left);
    Held.Y      = (int32_t)BoxShapePixelOf(S->Top);
    Held.Width  = (int32_t)(BoxShapePixelOf(S->Right) + 1 - Held.X);
    Held.Height = (int32_t)(BoxShapePixelOf((int64_t)S->Bottom + BOX_SHAPE_ONE - 1) - Held.Y);
    return BoxRectMeet(Held, Within);
}

static inline int32_t BoxShapeEdgeTop(const BoxShapeEdge *E) { return E->Y0 < E->Y1 ? E->Y0 : E->Y1; }
static inline int32_t BoxShapeEdgeBottom(const BoxShapeEdge *E) { return E->Y0 < E->Y1 ? E->Y1 : E->Y0; }

static inline void BoxShapeSwap(BoxShapeEdge *A, BoxShapeEdge *B)
{
    const BoxShapeEdge Held = *A;
    *A                      = *B;
    *B                      = Held;
}

/* The heap below Root, of Count edges, put back in order. */
static inline void BoxShapeSift(BoxShapeEdge *Edges, uint32_t Root, uint32_t Count)
{
    for (;;) {
        uint32_t Child = 2u * Root + 1u;
        if (Child >= Count) return;
        if (Child + 1u < Count && BoxShapeEdgeTop(&Edges[Child + 1u]) > BoxShapeEdgeTop(&Edges[Child])) Child++;
        if (BoxShapeEdgeTop(&Edges[Root]) >= BoxShapeEdgeTop(&Edges[Child])) return;
        BoxShapeSwap(&Edges[Root], &Edges[Child]);
        Root = Child;
    }
}

/* The shape's edges in the order their tops come, in their own room. */
static inline void BoxShapeSort(BoxShape *S)
{
    const uint32_t Count = S->Count;
    for (uint32_t I = Count / 2u; I-- > 0;) BoxShapeSift(S->Edges, I, Count);
    for (uint32_t End = Count; End-- > 1u;) {
        BoxShapeSwap(&S->Edges[0], &S->Edges[End]);
        BoxShapeSift(S->Edges, 0, End);
    }
}

/*
 * A shape swept row by row down its columns Left to End: each row's cells
 * laid by the edges that cross it alone — the edges sorted by their tops, the
 * ones begun above a row and not ended standing between Done and Next — so a
 * row costs its own edges, however many the shape has. The cells are those
 * every edge laid on its own would make: integers added, in any order.
 */
typedef struct BoxShapeSweep {
    BoxShape *Shape;
    int32_t  *Cells;   /* room for End - Left + 2 */
    int32_t   Left, End;
    uint32_t  Done;    /* the edges before this one ended above the row */
    uint32_t  Next;    /* the edges from this one begin below it */
} BoxShapeSweep;

static inline void BoxShapeSweepBegin(BoxShapeSweep *W, BoxShape *S, int32_t *Cells, int32_t Left, int32_t End)
{
    BoxShapeSort(S);
    W->Shape = S;
    W->Cells = Cells;
    W->Left  = Left;
    W->End   = End;
    W->Done  = 0;
    W->Next  = 0;
}

/* Row Y's cells — Y below every row swept before it: pixel X covers
 * |Cells[0] + Cells[1] + … + Cells[1 + X - Left]| of ONE·ONE, the shape's
 * rule saying how much of that it holds (BoxShapeCoverage). */
static inline void BoxShapeSweepRow(BoxShapeSweep *W, int32_t Y)
{
    const int64_t Top = (int64_t)Y * BOX_SHAPE_ONE, Bottom = Top + BOX_SHAPE_ONE;
    BoxShapeEdge *Edges = W->Shape->Edges;
    while (W->Next < W->Shape->Count && BoxShapeEdgeTop(&Edges[W->Next]) < Bottom) W->Next++;
    for (uint32_t I = W->Done; I < W->Next; I++)
        if (BoxShapeEdgeBottom(&Edges[I]) <= Top) BoxShapeSwap(&Edges[I], &Edges[W->Done++]);
    for (int32_t C = 0; C <= W->End - W->Left; C++) W->Cells[C] = 0;
    for (uint32_t I = W->Done; I < W->Next; I++) BoxShapeRowEdge(W->Cells, W->Left, W->End, &Edges[I], Y);
}

/* The running sum of a row's cells as the share of 255 of the pixel the
 * shape holds by its rule. */
static inline uint32_t BoxShapeCoverage(int64_t Run, uint32_t Rule)
{
    const int64_t Whole   = (int64_t)BOX_SHAPE_ONE * BOX_SHAPE_ONE;
    int64_t       Covered = BoxShapeSize(Run);
    if (Rule == BOX_SHAPE_EVENODD) {
        Covered %= 2 * Whole;
        if (Covered > Whole) Covered = 2 * Whole - Covered;
    } else if (Covered > Whole) {
        Covered = Whole;
    }
    return (uint32_t)((Covered * BOX_SHAPE_FULL + 32768) >> 16);
}

#endif /* BOXOS_SHAPE_H */
