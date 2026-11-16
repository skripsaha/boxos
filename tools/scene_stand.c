/*
 * scene_stand — the brush (src/include/boxos_brush.h) and scenes
 * (boxos_scene.h, boxos_scene_play.h) on the host: the very headers Gangway
 * and the display paint and play with, built under the address and
 * undefined-behaviour sanitizers and held to what they must do.
 *
 *     scene_stand brush SHEET.png
 *     scene_stand frames SCENE OUTDIR
 *     scene_stand first SCENE WIDTH HEIGHT OUT.ppm
 *
 * brush: numbers held to the host's own (the roots, the sine); colour laid as
 * the imprint is pressed, and text drawn the very pixels BoxPressGlyph makes;
 * every shape's coverage summed to its exact area — rectangles at any
 * fraction of a pixel, ellipses, rings, holes, lines, curves, turns; a circle
 * its own mirror and a shape moved by whole pixels the same pixels moved;
 * gradients' ends, a picture laid one to one; random fills under random
 * matrices never touching a pixel past the clip; what spills refused. Then a
 * sheet of everything it draws, SHEET.png, to look at.
 *
 * frames: the packed scene read; its first frame on screens from 640x480 to
 * 3840x2160 and standing up — the same picture on each, the corners the
 * paper; every figure drawn whole on a screen of 8K through enter and wait;
 * every frame of enter, of a round and more of wait, and of leave begun in
 * the middle of a round, drawn only where it changed and held to the same
 * frame drawn whole, pixel for pixel; enter's last frame wait's first; leave
 * ending in one colour, nothing of the scene left; the scene and its paths
 * damaged one named way at a
 * time refused for what is wrong, and thousands of random damages read and
 * played without a byte read or drawn past where it may be. Then a few of its
 * frames, PNG files in OUTDIR, to look at.
 *
 * first: the scene's first frame on a screen of WIDTH x HEIGHT, as a PPM — the
 * form QEMU's screendump writes, so what Gangway left on a machine's glass is
 * held to it byte for byte.
 *
 * Every check says "  ok   …" or "  FAIL …"; the last line counts them.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boxos_brush.h"
#include "boxos_imprint_data.h"
#include "boxos_imprint_press.h"
#include "boxos_scene_play.h"

#define ONE  BOX_BRUSH_ONE
#define UNIT BOX_BRUSH_UNIT

static int g_passed, g_failed;

static void Check(int Held, const char *What)
{
    if (Held) g_passed++;
    else      g_failed++;
    printf("  %s %s\n", Held ? "ok  " : "FAIL", What);
}

/* ---------------------------------------------------------------------------
 * A board: a canvas inside a border no pixel of it may reach.
 * ------------------------------------------------------------------------- */

#define BORDER   16
#define SENTINEL 0x00A5C3E1u

typedef struct {
    uint32_t      *Whole;   /* the canvas and its border */
    BoxBrushCanvas Canvas;
} Board;

static BoxBrushEdge g_edges[1u << 16];

static void BoardMake(Board *B, uint32_t Width, uint32_t Height)
{
    const uint32_t Stride = Width + 2u * BORDER;
    B->Whole = malloc((size_t)Stride * (Height + 2u * BORDER) * sizeof(uint32_t));
    if (!B->Whole) exit(2);
    for (size_t I = 0; I < (size_t)Stride * (Height + 2u * BORDER); I++) B->Whole[I] = SENTINEL;
    B->Canvas.Pixels = B->Whole + (size_t)BORDER * Stride + BORDER;
    B->Canvas.Width  = Width;
    B->Canvas.Height = Height;
    B->Canvas.Stride = Stride;
    B->Canvas.Clip   = (BoxBrushRect){ 0, 0, (int32_t)Width, (int32_t)Height };
    B->Canvas.Row    = malloc((Width + 2u) * sizeof(int32_t));
    if (!B->Canvas.Row) exit(2);
}

static void BoardFree(Board *B)
{
    free(B->Whole);
    free(B->Canvas.Row);
}

static uint32_t *At(Board *B, int32_t X, int32_t Y)
{
    return B->Canvas.Pixels + (size_t)Y * B->Canvas.Stride + X;
}

static void BoardClear(Board *B, uint32_t Color)
{
    for (uint32_t Y = 0; Y < B->Canvas.Height; Y++)
        for (uint32_t X = 0; X < B->Canvas.Width; X++) *At(B, (int32_t)X, (int32_t)Y) = Color;
}

/* Every pixel of the border and of the canvas past Clip still the sentinel. */
static int Untouched(Board *B, BoxBrushRect Clip)
{
    const uint32_t Stride = B->Canvas.Stride, Rows = B->Canvas.Height + 2u * BORDER;
    for (uint32_t Y = 0; Y < Rows; Y++) {
        for (uint32_t X = 0; X < Stride; X++) {
            const int32_t CX = (int32_t)X - BORDER, CY = (int32_t)Y - BORDER;
            const int     In = CX >= Clip.X && CY >= Clip.Y && CX < Clip.X + Clip.Width && CY < Clip.Y + Clip.Height;
            if (!In && B->Whole[(size_t)Y * Stride + X] != SENTINEL) return 0;
        }
    }
    return 1;
}

static const BoxBrushMatrix g_plain = { UNIT, 0, 0, UNIT, 0, 0 };

static BoxBrushOutline Begin(const BoxBrushMatrix *M)
{
    BoxBrushOutline O;
    BoxBrushBegin(&O, g_edges, sizeof(g_edges) / sizeof(g_edges[0]), M);
    return O;
}

/* The outline's coverage of every pixel of the canvas, 0 to 255, and the area
 * it covers in pixels: what BoxBrushFill lays, read as it reads it. */
static double Coverages(Board *B, BoxBrushOutline *O, uint8_t *Map)
{
    const BoxBrushCanvas *K = &B->Canvas;
    double                Area = 0;
    if (Map) memset(Map, 0, (size_t)K->Width * K->Height);
    BoxBrushClose(O);
    const BoxBrushRect Held = BoxBrushReach(K, O);
    for (int32_t Y = Held.Y; Y < Held.Y + Held.Height; Y++) {
        BoxBrushRowCells(K, O, Y, Held.X, Held.X + Held.Width);
        int64_t Run = K->Row[0];
        for (int32_t X = Held.X; X < Held.X + Held.Width; X++) {
            Run += K->Row[1 + X - Held.X];
            const int64_t Covered = Run < 0 ? -Run : Run;
            Area += (double)(Covered > 65536 ? 65536 : Covered) / 65536.0;
            if (Map) Map[(size_t)Y * K->Width + X] = (uint8_t)BoxBrushCoverage(Run);
        }
    }
    return Area;
}

static uint32_t g_seed = 0x9E3779B9u;

static uint32_t Random(void)
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

static int32_t Within(int32_t Low, int32_t High)
{
    return Low + (int32_t)(Random() % (uint32_t)(High - Low + 1));
}

static int Near(double Got, double Want, double Share)
{
    return fabs(Got - Want) <= fabs(Want) * Share;
}

/* ---------------------------------------------------------------------------
 * Numbers
 * ------------------------------------------------------------------------- */

static void CheckNumbers(void)
{
    int Held = 1;
    for (uint32_t V = 0; V < 65536u; V++) {
        const uint32_t R = BoxBrushRoot(V);
        if (R * R > V || (R + 1u) * (R + 1u) <= V) Held = 0;
    }
    Check(Held, "BoxBrushRoot is the whole root of every value below 65536");

    Held = 1;
    for (uint32_t I = 0; I < 200000u; I++) {
        uint64_t V = ((uint64_t)Random() << 32) | Random();
        if (I % 4u == 1u) V >>= Random() % 64u;
        if (I == 0) V = ~0ull;
        if (I == 2) V = 0;
        const uint64_t R = BoxBrushRoot64(V);
        if (R > 0xFFFFFFFFull || R * R > V || (R < 0xFFFFFFFFull && (R + 1u) * (R + 1u) <= V)) Held = 0;
    }
    for (uint64_t R = 1; R < 0xFFFFFFFFull; R = R * 3u + 1u)
        if (BoxBrushRoot64(R * R) != R || BoxBrushRoot64(R * R - 1u) != R - 1u) Held = 0;
    Check(Held, "BoxBrushRoot64 is the whole root of 64-bit values, squares and their neighbours");

    Held = BoxBrushSine(0) == 0 && BoxBrushSine(16384) == UNIT && BoxBrushSine(32768) == 0 &&
           BoxBrushSine(49152) == -UNIT && BoxBrushCosine(0) == UNIT && BoxBrushCosine(32768) == -UNIT;
    int32_t Off = 0;
    for (uint32_t T = 0; T < BOX_BRUSH_TURN; T++) {
        const int32_t Want = (int32_t)lround(sin(2.0 * M_PI * T / BOX_BRUSH_TURN) * UNIT);
        const int32_t Diff = abs(BoxBrushSine(T) - Want);
        if (Diff > Off) Off = Diff;
        if (T <= 32768u && BoxBrushSine(T) != BoxBrushSine(32768u - T)) Held = 0;
        if (BoxBrushSine(T + 32768u) != -BoxBrushSine(T)) Held = 0;
    }
    char Line[160];
    snprintf(Line, sizeof(Line), "BoxBrushSine: exact at every quarter, its own mirror, within %d/65536 of sin()", Off);
    Check(Held && Off <= 1, Line);
}

/* ---------------------------------------------------------------------------
 * Laying colour, and text, as the imprint is pressed
 * ------------------------------------------------------------------------- */

static void CheckPress(void)
{
    int Held = 1;
    for (uint32_t I = 0; I < 20000u; I++) {
        const uint32_t Paper = Random() & 0xFFFFFFu, Ink = Random() & 0xFFFFFFu;
        uint32_t       Inks[BOXOS_IMPRINT_INKED + 1u];
        BoxPressInks(Paper, Ink, BOX_PIXEL_BGR, Inks);
        for (uint32_t Shade = 0; Shade <= BOXOS_IMPRINT_INKED; Shade++) {
            const uint32_t Share = BoxBrushShare(Shade * BOX_BRUSH_FULL / BOXOS_IMPRINT_INKED, 255, 255);
            const uint32_t Laid  = Share ? BoxBrushLay(Paper, Ink, Share) : Paper;
            if (Laid != Inks[Shade]) Held = 0;
        }
    }
    Check(Held, "a colour laid at an imprint's step is the pixel the press mixes");

    const char *Text = "BoxOS — Бокс ОС: пиксели, линии ✓ {}[]|~";
    Held             = 1;
    for (uint32_t Size = 0; Size < BoxSystemImprint.SizeCount; Size++) {
        const BoxImprintSize *Cell = &BoxSystemImprint.Sizes[Size];
        Board                 Brushed, Pressed;
        const uint32_t        Paper = 0x203040u, Ink = 0xF0E0A0u;
        BoardMake(&Brushed, 64u * Cell->Width, Cell->Height);
        BoardMake(&Pressed, 64u * Cell->Width, Cell->Height);
        BoardClear(&Brushed, Paper);
        BoardClear(&Pressed, Paper);
        const int32_t Across = BoxBrushText(&Brushed.Canvas, &BoxSystemImprint, Size, 0, 0, Text, strlen(Text),
                                            0xFF000000u | Ink, 255);
        uint32_t Inks[BOXOS_IMPRINT_INKED + 1u];
        BoxPressInks(Paper, Ink, BOX_PIXEL_BGR, Inks);
        size_t   Read = 0;
        uint32_t Col  = 0;
        while (Read < strlen(Text)) {
            uint32_t Character;
            Read += BoxUtf8Next(Text + Read, strlen(Text) - Read, &Character);
            BoxPressGlyph(&BoxSystemImprint, Size, BoxImprintFind(&BoxSystemImprint, Character),
                          (volatile uint8_t *)At(&Pressed, (int32_t)(Col++ * Cell->Width), 0),
                          Pressed.Canvas.Stride * 4u, Inks);
        }
        if (Across != (int32_t)(Col * Cell->Width)) Held = 0;
        for (uint32_t Y = 0; Y < Cell->Height; Y++)
            for (uint32_t X = 0; X < 64u * Cell->Width; X++)
                if (*At(&Brushed, (int32_t)X, (int32_t)Y) != *At(&Pressed, (int32_t)X, (int32_t)Y)) Held = 0;
        BoardFree(&Brushed);
        BoardFree(&Pressed);
    }
    Check(Held, "text drawn by the brush is the very pixels BoxPressGlyph presses, at every imprint size");

    Held = BoxBrushLay(0x123456u, 0xFF654321u, 255) == 0x654321u;
    for (uint32_t I = 0; I < 1000u && Held; I++) {
        const uint32_t Below = Random() & 0xFFFFFFu, Color = Random();
        uint32_t       Last  = Below & 0xFFu;
        for (uint32_t Share = 1; Share <= 255u; Share++) {
            const uint32_t Blue = BoxBrushLay(Below, Color, Share) & 0xFFu;
            if ((Color & 0xFFu) >= (Below & 0xFFu) ? Blue < Last : Blue > Last) Held = 0;
            Last = Blue;
        }
        if (Last != (Color & 0xFFu)) Held = 0;
    }
    Check(Held, "laying moves every channel steadily from what is beneath to the colour, and whole is the colour");
}

/* ---------------------------------------------------------------------------
 * Coverage: areas, mirrors, holes
 * ------------------------------------------------------------------------- */

static void CheckRects(void)
{
    Board B;
    BoardMake(&B, 256, 256);
    BoardClear(&B, 0x000000u);
    BoxBrushOutline     O     = Begin(&g_plain);
    const BoxBrushPaint Solid = { BOX_BRUSH_PAINT_SOLID, 0xFF30A0F0u, NULL, 0, g_plain, NULL, 0, 0, 0 };
    BoxBrushAddRect(&O, 10 * ONE, 12 * ONE, 40 * ONE, 30 * ONE, 0);
    int      Held  = BoxBrushFill(&B.Canvas, &O, &Solid, 255);
    uint32_t Count = 0;
    for (int32_t Y = 0; Y < 256; Y++)
        for (int32_t X = 0; X < 256; X++) {
            const int In = X >= 10 && X < 50 && Y >= 12 && Y < 42;
            if (*At(&B, X, Y) != (In ? 0x30A0F0u : 0u)) Held = 0;
            Count += In;
        }
    Check(Held && Count == 1200u && Untouched(&B, B.Canvas.Clip),
          "a rectangle on whole pixels covers exactly its 1200 pixels, its colour whole, nothing else");

    Held = 1;
    double Worst = 0;
    for (uint32_t I = 0; I < 2000u; I++) {
        const int32_t X = Within(0, 100 * ONE), Y = Within(0, 100 * ONE);
        const int32_t W = Within(1, 150 * ONE), H = Within(1, 150 * ONE);
        O = Begin(&g_plain);
        BoxBrushAddRect(&O, X, Y, W, H, I % 2u);
        const double Got = Coverages(&B, &O, NULL), Want = (double)W * H / 65536.0;
        const double Off = fabs(Got - Want);
        if (Off > Worst) Worst = Off;
        if (Off > 0.0005 + 4.0 * (H / ONE + 2) / 65536.0) Held = 0;
    }
    char Line[200];
    snprintf(Line, sizeof(Line),
             "2000 rectangles at any fraction of a pixel, either winding, cover their exact area (worst %.6f px)", Worst);
    Check(Held, Line);

    uint8_t *Map = malloc(256u * 256u);
    O            = Begin(&g_plain);
    BoxBrushAddRect(&O, 10 * ONE + ONE / 2, 20 * ONE, 20 * ONE, 10 * ONE, 0);
    Coverages(&B, &O, Map);
    Check(Map[25 * 256 + 10] == 128 && Map[25 * 256 + 11] == 255 && Map[25 * 256 + 30] == 128 &&
              Map[25 * 256 + 9] == 0 && Map[25 * 256 + 31] == 0,
          "a rectangle's edge through the middle of a pixel covers it by half, 128 of 255");

    /* A shape drawn twice, and a smaller one at any fraction inside it, in one
     * outline: every pixel the big one covers whole is covered once. */
    uint8_t *Once = malloc(256u * 256u);
    O             = Begin(&g_plain);
    BoxBrushAddRect(&O, 12 * ONE, 15 * ONE, 35 * ONE, 27 * ONE, 0);
    const double Single = Coverages(&B, &O, Once);
    O                   = Begin(&g_plain);
    BoxBrushAddRect(&O, 12 * ONE, 15 * ONE, 35 * ONE, 27 * ONE, 0);
    BoxBrushAddRect(&O, 12 * ONE, 15 * ONE, 35 * ONE, 27 * ONE, 0);
    BoxBrushAddEllipse(&O, 5000, 6000, 1700, 1200, 0);
    const double Twice = Coverages(&B, &O, Map);
    Check(fabs(Single - Twice) < 1e-9 && memcmp(Once, Map, 256u * 256u) == 0,
          "solid contours lying over each other cover each pixel the whole of one covers once");

    O = Begin(&g_plain);
    BoxBrushAddRect(&O, 20 * ONE, 20 * ONE, 60 * ONE, 40 * ONE, 0);
    BoxBrushAddRect(&O, 30 * ONE, 30 * ONE, 20 * ONE, 10 * ONE, 1);
    const double Rim = Coverages(&B, &O, Map);
    Check(fabs(Rim - (2400.0 - 200.0)) < 1e-9 && Map[35 * 256 + 40] == 0 && Map[25 * 256 + 40] == 255,
          "a contour wound the other way cuts its hole: the rim's area exactly, the hole bare");

    /* Two rectangles side by side at a fraction of a pixel, one outline: no seam. */
    O = Begin(&g_plain);
    BoxBrushAddRect(&O, 10 * ONE, 10 * ONE, 15 * ONE + 77, 20 * ONE, 0);
    BoxBrushAddRect(&O, 25 * ONE + 77, 10 * ONE, 15 * ONE, 20 * ONE, 0);
    Coverages(&B, &O, Map);
    Check(Map[15 * 256 + 25] == 255, "two shapes meeting inside a pixel in one outline leave no seam");

    free(Once);
    free(Map);
    BoardFree(&B);
}

static void CheckRound(void)
{
    Board B;
    BoardMake(&B, 300, 300);
    uint8_t *Map = malloc(300u * 300u);
    char     Line[220];

    BoxBrushOutline O = Begin(&g_plain);
    BoxBrushAddEllipse(&O, (int32_t)(100.4 * ONE), (int32_t)(80.7 * ONE), (int32_t)(37.3 * ONE),
                       (int32_t)(21.9 * ONE), 0);
    const double Ellipse = Coverages(&B, &O, NULL), Want = M_PI * 37.3 * 21.9;
    snprintf(Line, sizeof(Line), "an ellipse covers its area, %.2f of %.2f pixels", Ellipse, Want);
    Check(Near(Ellipse, Want, 0.001), Line);

    int Held = 1;
    for (uint32_t I = 0; I < 300u; I++) {
        const double R = 0.6 + (Random() % 12000u) / 100.0;
        O              = Begin(&g_plain);
        BoxBrushAddEllipse(&O, 150 * ONE + Within(0, 255), 150 * ONE + Within(0, 255), (int32_t)(R * ONE),
                           (int32_t)(R * ONE), I % 2u);
        const double Got = Coverages(&B, &O, NULL), Disc = M_PI * (double)((int32_t)(R * ONE)) * ((int32_t)(R * ONE)) / 65536.0;
        if (!Near(Got, Disc, R < 4 ? 0.03 : 0.002)) Held = 0;
    }
    Check(Held, "300 circles from under a pixel to 120 across, either winding, cover their area");

    O = Begin(&g_plain);
    BoxBrushAddEllipse(&O, 150 * ONE, 150 * ONE, 50 * ONE, 50 * ONE, 0);
    BoxBrushAddEllipse(&O, 150 * ONE, 150 * ONE, 30 * ONE, 30 * ONE, 1);
    const double Ring = Coverages(&B, &O, Map);
    Check(Near(Ring, M_PI * (2500.0 - 900.0), 0.002) && Map[150 * 300 + 150] == 0 && Map[150 * 300 + 110] == 255,
          "a ring: its area, its middle bare, its band whole");

    /* A circle about a pixel's corner: its own mirror across both axes
     * exactly. Rows are crossed where columns are summed, so across the
     * diagonal it may be a 255th apart. */
    O = Begin(&g_plain);
    BoxBrushAddEllipse(&O, 64 * ONE, 64 * ONE, (int32_t)(30.3 * ONE), (int32_t)(30.3 * ONE), 0);
    Coverages(&B, &O, Map);
    Held          = 1;
    int Diagonal  = 0;
    for (int32_t Y = 0; Y < 128; Y++)
        for (int32_t X = 0; X < 128; X++) {
            const int C = Map[Y * 300 + X];
            if (C != Map[Y * 300 + (127 - X)] || C != Map[(127 - Y) * 300 + X]) Held = 0;
            if (abs(C - Map[X * 300 + Y]) > Diagonal) Diagonal = abs(C - Map[X * 300 + Y]);
        }
    Check(Held && Diagonal <= 1,
          "a circle about a pixel's corner is exactly its own mirror across both axes, and across the diagonal to a 255th");

    /* The same circle moved by whole pixels: the same pixels, moved. */
    uint8_t *Moved = malloc(300u * 300u);
    O              = Begin(&g_plain);
    BoxBrushAddEllipse(&O, (int32_t)(60.37 * ONE), (int32_t)(70.81 * ONE), (int32_t)(25.5 * ONE), (int32_t)(18.2 * ONE), 0);
    Coverages(&B, &O, Map);
    BoxBrushMatrix Shift = g_plain;
    Shift.E              = 7 * ONE;
    Shift.F              = 3 * ONE;
    O                    = Begin(&Shift);
    BoxBrushAddEllipse(&O, (int32_t)(60.37 * ONE), (int32_t)(70.81 * ONE), (int32_t)(25.5 * ONE), (int32_t)(18.2 * ONE), 0);
    Coverages(&B, &O, Moved);
    Held = 1;
    for (int32_t Y = 0; Y < 200; Y++)
        for (int32_t X = 0; X < 200; X++)
            if (Map[Y * 300 + X] != Moved[(Y + 3) * 300 + X + 7]) Held = 0;
    Check(Held, "a shape moved by whole pixels is the same pixels, moved");

    /* A rounded rectangle: the rectangle less four corners' worth. */
    O = Begin(&g_plain);
    BoxBrushAddRounded(&O, 20 * ONE, 30 * ONE, 120 * ONE, 80 * ONE, 16 * ONE, 0);
    const double Rounded = Coverages(&B, &O, NULL), RoundedWant = 120.0 * 80.0 - (4.0 - M_PI) * 256.0;
    snprintf(Line, sizeof(Line), "a rounded rectangle covers its area, %.2f of %.2f", Rounded, RoundedWant);
    Check(Near(Rounded, RoundedWant, 0.001), Line);

    free(Moved);
    free(Map);
    BoardFree(&B);
}

static void CheckLinesCurves(void)
{
    Board B;
    BoardMake(&B, 320, 320);
    char Line[200];

    BoxBrushOutline O = Begin(&g_plain);
    BoxBrushAddLine(&O, 20 * ONE, 50 * ONE + ONE / 2, 120 * ONE, 50 * ONE + ONE / 2, 3 * ONE, 0);
    const double Butt = Coverages(&B, &O, NULL);
    O                 = Begin(&g_plain);
    BoxBrushAddLine(&O, 20 * ONE, 50 * ONE + ONE / 2, 120 * ONE, 50 * ONE + ONE / 2, 3 * ONE, 1);
    const double Round = Coverages(&B, &O, NULL);
    snprintf(Line, sizeof(Line), "a line 3 wide and 100 long: %.3f square-cut, %.3f round-ended", Butt, Round);
    Check(fabs(Butt - 300.0) < 1e-6 && Near(Round, 300.0 + M_PI * 2.25, 0.003), Line);

    int Held = 1;
    for (uint32_t I = 0; I < 400u; I++) {
        const int32_t X0 = Within(40 * ONE, 280 * ONE), Y0 = Within(40 * ONE, 280 * ONE);
        const int32_t X1 = Within(40 * ONE, 280 * ONE), Y1 = Within(40 * ONE, 280 * ONE);
        const int32_t W  = Within(ONE / 4, 12 * ONE);
        O                = Begin(&g_plain);
        BoxBrushAddLine(&O, X0, Y0, X1, Y1, W, 0);
        const double Length = hypot((double)(X1 - X0), (double)(Y1 - Y0)) / ONE;
        if (Length < 1) continue;
        /* A corner lands on the nearest place: the width may be half a place
         * out on each side. */
        const double Got = Coverages(&B, &O, NULL), Want = Length * (W / 2 * 2) / ONE;
        if (fabs(Got - Want) > Want * 0.002 + Length / ONE + 0.02) Held = 0;
    }
    Check(Held, "400 lines at any slope and width, a quarter pixel to twelve, cover length times width");

    /* Lines into one outline: a bent line covers its two halves less the
     * overlap at the bend — never twice. */
    O = Begin(&g_plain);
    BoxBrushAddLine(&O, 50 * ONE, 200 * ONE, 150 * ONE, 200 * ONE, 8 * ONE, 1);
    BoxBrushAddLine(&O, 150 * ONE, 200 * ONE, 150 * ONE, 300 * ONE, 8 * ONE, 1);
    /* Each covers 800 + 16π; they share the disc about the bend and the
     * corner of their bodies past it, 12π + 16. */
    const double Bent = Coverages(&B, &O, NULL), BentWant = 2.0 * (800.0 + M_PI * 16.0) - (M_PI * 12.0 + 16.0);
    snprintf(Line, sizeof(Line), "two round-ended lines meeting at a bend cover %.2f of %.2f — the bend once", Bent,
             BentWant);
    Check(Near(Bent, BentWant, 0.003), Line);

    /* A quadratic closed by its chord: two thirds of its control triangle. */
    O = Begin(&g_plain);
    BoxBrushMoveTo(&O, 20 * ONE, 150 * ONE);
    BoxBrushQuadTo(&O, 110 * ONE, 20 * ONE, 260 * ONE, 140 * ONE);
    const double Quad     = Coverages(&B, &O, NULL);
    const double Triangle = fabs((110.0 - 20.0) * (140.0 - 150.0) - (260.0 - 20.0) * (20.0 - 150.0)) / 2.0;
    snprintf(Line, sizeof(Line), "a quadratic curve closed by its chord covers %.2f of %.2f", Quad, Triangle * 2.0 / 3.0);
    Check(Near(Quad, Triangle * 2.0 / 3.0, 0.004), Line);

    /* Four cubic quarters: a circle, to the cubic's own 0.03%. */
    const double  K = 0.5522847498 * 100.0;
    const int32_t C = 160 * ONE, R = 100 * ONE, P = (int32_t)(K * ONE);
    O               = Begin(&g_plain);
    BoxBrushMoveTo(&O, C + R, C);
    BoxBrushCubicTo(&O, C + R, C + P, C + P, C + R, C, C + R);
    BoxBrushCubicTo(&O, C - P, C + R, C - R, C + P, C - R, C);
    BoxBrushCubicTo(&O, C - R, C - P, C - P, C - R, C, C - R);
    BoxBrushCubicTo(&O, C + P, C - R, C + R, C - P, C + R, C);
    const double Cubic = Coverages(&B, &O, NULL);
    snprintf(Line, sizeof(Line), "four cubic quarters cover the circle they stand for, %.2f of %.2f", Cubic, M_PI * 10000.0);
    Check(Near(Cubic, M_PI * 10000.0, 0.003), Line);

    BoardFree(&B);
}

static void CheckTurns(void)
{
    Board B;
    BoardMake(&B, 200, 200);
    uint8_t *Plain = malloc(200u * 200u), *Turned = malloc(200u * 200u);
    char     Line[200];

    BoxBrushMatrix  M = BoxBrushPlaced(100 * ONE, 100 * ONE, UNIT, 0);
    BoxBrushOutline O = Begin(&M);
    BoxBrushAddRect(&O, -20 * ONE, -20 * ONE, 40 * ONE, 40 * ONE, 0);
    Coverages(&B, &O, Plain);
    M = BoxBrushPlaced(100 * ONE, 100 * ONE, UNIT, BOX_BRUSH_TURN / 4u);
    O = Begin(&M);
    BoxBrushAddRect(&O, -20 * ONE, -20 * ONE, 40 * ONE, 40 * ONE, 0);
    Coverages(&B, &O, Turned);
    Check(memcmp(Plain, Turned, 200u * 200u) == 0, "a square turned a quarter about its middle is the same pixels");

    int    Held  = 1;
    double Worst = 0;
    for (uint32_t T = 0; T < BOX_BRUSH_TURN; T += 997u) {
        M = BoxBrushPlaced(100 * ONE + 37, 100 * ONE + 211, (int32_t)(1.5 * UNIT), T);
        O = Begin(&M);
        BoxBrushAddRect(&O, -20 * ONE, -10 * ONE, 40 * ONE, 20 * ONE, 0);
        const double Got = Coverages(&B, &O, NULL), Off = fabs(Got - 1800.0) / 1800.0;
        if (Off > Worst) Worst = Off;
        if (Off > 0.0005) Held = 0;
    }
    snprintf(Line, sizeof(Line), "a rectangle stretched by 1.5 keeps its area at 66 turns (worst %.4f%%)", Worst * 100.0);
    Check(Held, Line);

    const BoxBrushMatrix Half  = { UNIT / 2, 0, 0, UNIT / 2, 10 * ONE, 10 * ONE };
    const BoxBrushMatrix Twice = BoxBrushThen(Half, BoxBrushPlaced(50 * ONE, 0, 2 * UNIT, 0));
    Check(Twice.A == UNIT && Twice.D == UNIT && Twice.B == 0 && Twice.C == 0 && Twice.E == 70 * ONE &&
              Twice.F == 20 * ONE,
          "matrices made one: halved, then doubled and moved, is moved");

    free(Plain);
    free(Turned);
    BoardFree(&B);
}

/* ---------------------------------------------------------------------------
 * Paints
 * ------------------------------------------------------------------------- */

static int ChannelsNear(uint32_t A, uint32_t B, uint32_t Off)
{
    for (uint32_t S = 0; S <= 16u; S += 8u)
        if ((uint32_t)abs((int)((A >> S) & 0xFFu) - (int)((B >> S) & 0xFFu)) > Off) return 0;
    return 1;
}

static void CheckPaints(void)
{
    Board B;
    BoardMake(&B, 300, 200);
    BoardClear(&B, 0);
    const BoxBrushStop Stops[2] = { { 0, 0xFFFF2000u }, { UNIT, 0xFF0040FFu } };

    BoxBrushPaint   Linear = { BOX_BRUSH_PAINT_LINEAR, 0, Stops, 2, { 51200, 0, 0, UNIT, 50 * ONE, 0 }, NULL, 0, 0, 0 };
    BoxBrushOutline O      = Begin(&g_plain);
    BoxBrushAddRect(&O, 0, 0, 300 * ONE, 20 * ONE, 0);
    int Held = BoxBrushFill(&B.Canvas, &O, &Linear, 255);
    Check(Held && *At(&B, 10, 5) == 0xFF2000u && *At(&B, 280, 5) == 0x0040FFu &&
              ChannelsNear(*At(&B, 150, 5), BoxBrushBetween(Stops[0].Color, Stops[1].Color, UNIT / 2), 1),
          "a linear gradient: its first colour before it, its last past it, halfway between at its middle");

    BoxBrushPaint Radial = { BOX_BRUSH_PAINT_RADIAL, 0, Stops, 2, { 12800, 0, 0, 12800, 150 * ONE + 128, 120 * ONE + 128 },
                             NULL, 0, 0, 0 };
    O                    = Begin(&g_plain);
    BoxBrushAddRect(&O, 0, 30 * ONE, 300 * ONE, 170 * ONE, 0);
    Held = BoxBrushFill(&B.Canvas, &O, &Radial, 255);
    Check(Held && *At(&B, 150, 120) == 0xFF2000u && *At(&B, 150, 190) == 0x0040FFu && *At(&B, 10, 40) == 0x0040FFu &&
              ChannelsNear(*At(&B, 175, 120), BoxBrushBetween(Stops[0].Color, Stops[1].Color, UNIT / 2), 2),
          "a radial gradient: its first colour at its middle, its last past its radius, halfway between halfway out");

    uint32_t Picture[7 * 5];
    for (uint32_t I = 0; I < 35u; I++) Picture[I] = 0xFF000000u | (Random() & 0xFFFFFFu);
    BoxBrushPaint Laid = { BOX_BRUSH_PAINT_PICTURE, 0, NULL, 0, { UNIT, 0, 0, UNIT, 30 * ONE, 40 * ONE }, Picture, 7, 5, 7 };
    O                  = Begin(&g_plain);
    BoxBrushAddRect(&O, 30 * ONE, 40 * ONE, 7 * ONE, 5 * ONE, 0);
    Held = BoxBrushFill(&B.Canvas, &O, &Laid, 255);
    for (int32_t Y = 0; Y < 5; Y++)
        for (int32_t X = 0; X < 7; X++)
            if (*At(&B, 30 + X, 40 + Y) != (Picture[Y * 7 + X] & 0xFFFFFFu)) Held = 0;
    Check(Held, "a picture laid one to one on whole pixels is its very pixels");

    Picture[0]  = 0x80FFFFFFu;
    const uint32_t Below = 0x102030u;
    *At(&B, 30, 40)      = Below;
    O                    = Begin(&g_plain);
    BoxBrushAddRect(&O, 30 * ONE, 40 * ONE, ONE, ONE, 0);
    BoxBrushFill(&B.Canvas, &O, &Laid, 255);
    Check(*At(&B, 30, 40) == BoxBrushLay(Below, 0xFFFFFFu, 0x80), "a picture's half-clear pixel is laid at its alpha");

    BoardFree(&B);
}

/* ---------------------------------------------------------------------------
 * The clip, and what is refused
 * ------------------------------------------------------------------------- */

static BoxBrushMatrix RandomMatrix(int32_t Width, int32_t Height)
{
    const int32_t Stretch = (int32_t)(UNIT >> 6) << (Random() % 13u);
    BoxBrushMatrix M      = BoxBrushPlaced(Within(-Width, 2 * Width) * ONE + Within(0, 255),
                                           Within(-Height, 2 * Height) * ONE + Within(0, 255), Stretch, Random());
    if (Random() % 4u == 0) M.C = Within(-UNIT, UNIT);
    return M;
}

static void CheckClip(void)
{
    Board B;
    BoardMake(&B, 160, 120);
    const BoxBrushRect Clip = { 20, 15, 100, 80 };
    B.Canvas.Clip           = Clip;
    uint32_t           Picture[16 * 9];
    for (uint32_t I = 0; I < 16u * 9u; I++) Picture[I] = Random();
    BoxBrushStop Stops[3] = { { 0, Random() }, { UNIT / 3, Random() }, { UNIT, Random() } };
    uint32_t     Filled = 0, Refused = 0;
    for (uint32_t I = 0; I < 30000u; I++) {
        const BoxBrushMatrix M    = RandomMatrix(160, 120);
        BoxBrushOutline      O    = Begin(&M);
        const int32_t        Size = Within(1, 400) * ONE / 4;
        switch (Random() % 6u) {
        case 0: BoxBrushAddRect(&O, -Size, -Size / 2, 2 * Size, Size, Random() % 2u); break;
        case 1: BoxBrushAddEllipse(&O, 0, 0, Size, Size / 3 + 1, Random() % 2u); break;
        case 2: BoxBrushAddRounded(&O, -Size, -Size, 2 * Size, Size, Size / 4, Random() % 2u); break;
        case 3: BoxBrushAddLine(&O, -Size, 0, Size, Size / 2, Size / 5, Random() % 2u); break;
        case 4:
            BoxBrushMoveTo(&O, -Size, 0);
            BoxBrushCubicTo(&O, -Size, -Size, Size, Size, Size, 0);
            BoxBrushQuadTo(&O, 0, Size, -Size, 0);
            break;
        default:
            BoxBrushMoveTo(&O, 0, 0);
            for (uint32_t K = 0; K < 9u; K++) BoxBrushLineTo(&O, Within(-Size, Size), Within(-Size, Size));
        }
        BoxBrushPaint P = { Random() % 4u, Random(), Stops, 3, RandomMatrix(160, 120), Picture, 16, 9, 16 };
        BoxBrushFill(&B.Canvas, &O, &P, Random() % 256u) ? Filled++ : Refused++;
        if (I % 7u == 0) BoxBrushText(&B.Canvas, &BoxSystemImprint, Random() % 3u, Within(-40, 170), Within(-40, 130),
                                      "clip ✓", 8, Random(), 255);
        if (I % 5u == 0) BoxBrushPixel(&B.Canvas, Within(-5, 165), Within(-5, 125), Random(), Random() % 256u);
    }
    char Line[200];
    snprintf(Line, sizeof(Line),
             "30000 random fills, text and pixels under random matrices touch no pixel past the clip (%u filled, %u refused)",
             Filled, Refused);
    Check(Untouched(&B, Clip) && Filled > 20000u, Line);
    BoardFree(&B);
}

static void CheckRefused(void)
{
    Board B;
    BoardMake(&B, 64, 64);
    BoardClear(&B, 0x111111u);
    const BoxBrushPaint Solid = { BOX_BRUSH_PAINT_SOLID, 0xFFFFFFFFu, NULL, 0, g_plain, NULL, 0, 0, 0 };
    BoxBrushEdge        Few[4];
    BoxBrushOutline     O;

    BoxBrushBegin(&O, Few, 4, &g_plain);
    BoxBrushAddEllipse(&O, 32 * ONE, 32 * ONE, 20 * ONE, 20 * ONE, 0);
    const int Room = BoxBrushFill(&B.Canvas, &O, &Solid, 255);

    const BoxBrushMatrix Wide = { (BOX_BRUSH_STRETCH_MOST + 1) * UNIT, 0, 0, UNIT, 0, 0 };
    O                         = Begin(&Wide);
    BoxBrushAddRect(&O, 0, 0, ONE, ONE, 0);
    const int Stretch = BoxBrushFill(&B.Canvas, &O, &Solid, 255);

    O = Begin(&g_plain);
    BoxBrushAddRect(&O, 0, 0, (BOX_BRUSH_REACH + 1) * ONE, ONE, 0);
    const int Reach = BoxBrushFill(&B.Canvas, &O, &Solid, 255);

    O = Begin(&g_plain);
    BoxBrushLineTo(&O, 10 * ONE, 10 * ONE);
    const int Unbegun = BoxBrushFill(&B.Canvas, &O, &Solid, 255);

    O = Begin(&g_plain);
    BoxBrushAddLine(&O, -2147483647, 0, 2147483647, 0, 2147483647, 1);
    BoxBrushAddEllipse(&O, 0, 0, 2147483647, 2147483647, 0);
    const int Own    = BoxBrushFill(&B.Canvas, &O, &Solid, 255);
    const int NoSize = BoxBrushText(&B.Canvas, &BoxSystemImprint, BoxSystemImprint.SizeCount, 0, 0, "x", 1,
                                    0xFFFFFFFFu, 255);

    const BoxBrushStop  Stop   = { 0, 0xFFFFFFFFu };
    const BoxBrushPaint Folded = { BOX_BRUSH_PAINT_RADIAL, 0, &Stop, 1, { UNIT, UNIT, UNIT, UNIT, 0, 0 }, NULL, 0, 0, 0 };
    O                          = Begin(&g_plain);
    BoxBrushAddRect(&O, 0, 0, 10 * ONE, 10 * ONE, 0);
    const int Flat = BoxBrushFill(&B.Canvas, &O, &Folded, 255);

    int Bare = 1;
    for (int32_t Y = 0; Y < 64; Y++)
        for (int32_t X = 0; X < 64; X++)
            if (*At(&B, X, Y) != 0x111111u) Bare = 0;
    Check(!Room && !Stretch && !Reach && !Unbegun && !Own && !NoSize && !Flat && Bare,
          "refused, and nothing drawn: edges past their room, a matrix stretching too far, a place past reach, an "
          "edge with no contour begun, a shape's own place past its most, a size the imprint has not, a paint "
          "folded flat");
    BoardFree(&B);
}

/* ---------------------------------------------------------------------------
 * The sheet
 * ------------------------------------------------------------------------- */

static void Put32(uint8_t *At, uint32_t V)
{
    At[0] = (uint8_t)(V >> 24);
    At[1] = (uint8_t)(V >> 16);
    At[2] = (uint8_t)(V >> 8);
    At[3] = (uint8_t)V;
}

static uint32_t Crc(const uint8_t *Bytes, size_t N, uint32_t Crc)
{
    Crc = ~Crc;
    for (size_t I = 0; I < N; I++) {
        Crc ^= Bytes[I];
        for (int K = 0; K < 8; K++) Crc = (Crc >> 1) ^ (0xEDB88320u & (0u - (Crc & 1u)));
    }
    return ~Crc;
}

static void Chunk(FILE *Out, const char *Kind, const uint8_t *Data, size_t N)
{
    uint8_t Head[8];
    Put32(Head, (uint32_t)N);
    memcpy(Head + 4, Kind, 4);
    fwrite(Head, 1, 8, Out);
    if (N) fwrite(Data, 1, N, Out);
    uint8_t Tail[4];
    Put32(Tail, Crc(Data, N, Crc((const uint8_t *)Kind, 4, 0)));
    fwrite(Tail, 1, 4, Out);
}

/* The canvas as a PNG — its rows deflated as stored blocks. */
static int WritePng(const char *Path, const BoxBrushCanvas *K)
{
    const size_t RowBytes = 1u + 3u * K->Width, Raw = RowBytes * K->Height;
    const size_t Blocks = (Raw + 65534u) / 65535u;
    uint8_t     *Rows   = malloc(Raw);
    uint8_t     *Z      = malloc(2u + Raw + 5u * Blocks + 4u);
    if (!Rows || !Z) return 0;
    for (uint32_t Y = 0; Y < K->Height; Y++) {
        uint8_t *Line = Rows + Y * RowBytes;
        Line[0]       = 0;
        for (uint32_t X = 0; X < K->Width; X++) {
            const uint32_t P = K->Pixels[(size_t)Y * K->Stride + X];
            Line[1 + 3 * X]  = (uint8_t)(P >> 16);
            Line[2 + 3 * X]  = (uint8_t)(P >> 8);
            Line[3 + 3 * X]  = (uint8_t)P;
        }
    }
    size_t   N = 0;
    uint32_t A = 1, B = 0;
    Z[N++]     = 0x78;
    Z[N++]     = 0x01;
    for (size_t At = 0; At < Raw; At += 65535u) {
        const size_t Take = Raw - At < 65535u ? Raw - At : 65535u;
        Z[N++]            = At + Take == Raw;
        Z[N++]            = (uint8_t)Take;
        Z[N++]            = (uint8_t)(Take >> 8);
        Z[N++]            = (uint8_t)~Take;
        Z[N++]            = (uint8_t)(~Take >> 8);
        memcpy(Z + N, Rows + At, Take);
        N += Take;
    }
    for (size_t I = 0; I < Raw; I++) {
        A = (A + Rows[I]) % 65521u;
        B = (B + A) % 65521u;
    }
    Put32(Z + N, (B << 16) | A);
    N += 4;
    FILE *Out = fopen(Path, "wb");
    if (!Out) return 0;
    static const uint8_t Signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    uint8_t              Header[13];
    Put32(Header, K->Width);
    Put32(Header + 4, K->Height);
    Header[8]  = 8;
    Header[9]  = 2;
    Header[10] = Header[11] = Header[12] = 0;
    fwrite(Signature, 1, 8, Out);
    Chunk(Out, "IHDR", Header, 13);
    Chunk(Out, "IDAT", Z, N);
    Chunk(Out, "IEND", NULL, 0);
    const int Written = fclose(Out) == 0;
    free(Rows);
    free(Z);
    return Written;
}

static void Fill(Board *B, BoxBrushOutline *O, uint32_t Color, uint32_t Opacity)
{
    const BoxBrushPaint P = { BOX_BRUSH_PAINT_SOLID, Color, NULL, 0, g_plain, NULL, 0, 0, 0 };
    BoxBrushFill(&B->Canvas, O, &P, Opacity);
}

static void Words(Board *B, uint32_t Size, int32_t X, int32_t Y, const char *Text, uint32_t Color)
{
    BoxBrushText(&B->Canvas, &BoxSystemImprint, Size, X, Y, Text, strlen(Text), Color, 255);
}

static int DrawSheet(const char *Path)
{
    Board B;
    BoardMake(&B, 1280, 800);

    const BoxBrushStop Sky[3] = { { 0, 0xFF0E1428u }, { UNIT / 2, 0xFF1C1A3Cu }, { UNIT, 0xFF2A1430u } };
    /* The gradient's way runs down the sheet: its x is the sheet's y. */
    const BoxBrushPaint Back = { BOX_BRUSH_PAINT_LINEAR, 0, Sky, 3, { 0, 800 * ONE, -UNIT, 0, 0, 0 }, NULL, 0, 0, 0 };
    BoxBrushOutline     O    = Begin(&g_plain);
    BoxBrushAddRect(&O, 0, 0, 1280 * ONE, 800 * ONE, 0);
    BoxBrushFill(&B.Canvas, &O, &Back, 255);

    Words(&B, 2, 24, 16, "BoxOS brush — пиксели, линии, прямоугольники, круги, кривые, градиенты, картинки", 0xFFF2EEE6u);

    /* Shapes along the top. */
    O = Begin(&g_plain);
    BoxBrushAddRect(&O, 30 * ONE, 70 * ONE, 110 * ONE, 80 * ONE, 0);
    Fill(&B, &O, 0xFFE8604Cu, 255);
    O = Begin(&g_plain);
    BoxBrushAddRounded(&O, 160 * ONE + 128, 70 * ONE + 64, 110 * ONE, 80 * ONE, 22 * ONE, 0);
    Fill(&B, &O, 0xFFF0B030u, 255);
    O = Begin(&g_plain);
    BoxBrushAddRounded(&O, 290 * ONE, 70 * ONE, 110 * ONE, 80 * ONE, 14 * ONE, 0);
    BoxBrushAddRounded(&O, 296 * ONE, 76 * ONE, 98 * ONE, 68 * ONE, 8 * ONE, 1);
    Fill(&B, &O, 0xFF60D090u, 255);
    O = Begin(&g_plain);
    BoxBrushAddEllipse(&O, 475 * ONE, 110 * ONE, 55 * ONE, 40 * ONE, 0);
    Fill(&B, &O, 0xFF4CA8E8u, 255);
    O = Begin(&g_plain);
    BoxBrushAddEllipse(&O, 600 * ONE, 110 * ONE, 42 * ONE, 42 * ONE, 0);
    BoxBrushAddEllipse(&O, 600 * ONE, 110 * ONE, 28 * ONE, 28 * ONE, 1);
    Fill(&B, &O, 0xFFB070E0u, 255);
    BoxBrushMatrix M = BoxBrushPlaced(720 * ONE, 110 * ONE, UNIT, BOX_BRUSH_TURN / 16u);
    O                = Begin(&M);
    BoxBrushAddRect(&O, -35 * ONE, -35 * ONE, 70 * ONE, 70 * ONE, 0);
    Fill(&B, &O, 0xFFE070A0u, 255);

    /* A star, a heart and a leaf. */
    O = Begin(&g_plain);
    for (uint32_t K = 0; K < 10u; K++) {
        const uint32_t Turn = K * BOX_BRUSH_TURN / 10u + BOX_BRUSH_TURN * 3u / 4u;
        const int32_t  R    = (K % 2u ? 20 : 48) * ONE;
        const int32_t  X    = 840 * ONE + (int32_t)(((int64_t)R * BoxBrushCosine(Turn)) >> 16);
        const int32_t  Y    = 112 * ONE + (int32_t)(((int64_t)R * BoxBrushSine(Turn)) >> 16);
        if (K == 0) BoxBrushMoveTo(&O, X, Y);
        else        BoxBrushLineTo(&O, X, Y);
    }
    Fill(&B, &O, 0xFFF8E060u, 255);
    O = Begin(&g_plain);
    BoxBrushMoveTo(&O, 960 * ONE, 150 * ONE);
    BoxBrushCubicTo(&O, 900 * ONE, 110 * ONE, 915 * ONE, 60 * ONE, 960 * ONE, 88 * ONE);
    BoxBrushCubicTo(&O, 1005 * ONE, 60 * ONE, 1020 * ONE, 110 * ONE, 960 * ONE, 150 * ONE);
    Fill(&B, &O, 0xFFF04870u, 255);
    O = Begin(&g_plain);
    BoxBrushMoveTo(&O, 1050 * ONE, 150 * ONE);
    BoxBrushQuadTo(&O, 1050 * ONE, 70 * ONE, 1140 * ONE, 70 * ONE);
    BoxBrushQuadTo(&O, 1140 * ONE, 150 * ONE, 1050 * ONE, 150 * ONE);
    Fill(&B, &O, 0xFF70D060u, 255);

    /* A fan of lines, widths a pixel to six, and hairlines at shallow slopes. */
    for (uint32_t K = 0; K < 24u; K++) {
        const uint32_t Turn = K * (BOX_BRUSH_TURN / 2u) / 24u;
        const int32_t  X    = 150 * ONE + (int32_t)(((int64_t)120 * ONE * BoxBrushCosine(Turn)) >> 16);
        const int32_t  Y    = 330 * ONE - (int32_t)(((int64_t)120 * ONE * BoxBrushSine(Turn)) >> 16);
        O                   = Begin(&g_plain);
        BoxBrushAddLine(&O, 150 * ONE, 330 * ONE, X, Y, (int32_t)(1 + K % 6u) * ONE, 1);
        Fill(&B, &O, 0xFFDDE6F0u, 255);
    }
    for (uint32_t K = 0; K < 12u; K++) {
        O = Begin(&g_plain);
        BoxBrushAddLine(&O, 300 * ONE, (200 + 12 * K) * ONE, 560 * ONE, (200 + 12 * K) * ONE + (int32_t)K * 40, ONE, 0);
        Fill(&B, &O, 0xFFDDE6F0u, 255);
    }

    /* Spheres from radial gradients, Duppo's way. */
    const BoxBrushStop Shine[3] = { { 0, 0xFFFFF0FAu }, { UNIT / 2, 0xFFE89AD0u }, { UNIT, 0xFF9A5AC0u } };
    for (uint32_t K = 0; K < 3u; K++) {
        const int32_t CX = (660 + 130 * (int32_t)K) * ONE, CY = 300 * ONE, R = (30 + 20 * (int32_t)K) * ONE;
        const int32_t G  = (int32_t)((int64_t)R * 2 * UNIT / BOX_BRUSH_GRADIENT);
        BoxBrushPaint P  = { BOX_BRUSH_PAINT_RADIAL, 0, Shine, 3, { G, 0, 0, G, CX - R / 3, CY - R / 3 }, NULL, 0, 0, 0 };
        O                = Begin(&g_plain);
        BoxBrushAddEllipse(&O, CX, CY, R, R, 0);
        BoxBrushFill(&B.Canvas, &O, &P, 255);
    }

    /* Translucent discs over each other. */
    const uint32_t Lights[3] = { 0xFFFF4040u, 0xFF40FF60u, 0xFF4080FFu };
    for (uint32_t K = 0; K < 3u; K++) {
        O = Begin(&g_plain);
        BoxBrushAddEllipse(&O, (1080 + (K == 1 ? 40 : K == 2 ? -40 : 0)) * ONE, (280 + (K ? 60 : 0)) * ONE, 60 * ONE,
                           60 * ONE, 0);
        Fill(&B, &O, Lights[K], 140);
    }

    /* A picture: one to one, four times, and turned. */
    uint32_t Picture[16 * 16];
    for (uint32_t Y = 0; Y < 16u; Y++)
        for (uint32_t X = 0; X < 16u; X++)
            Picture[Y * 16u + X] = ((X + Y) % 2u ? 0xFF000000u : 0xFF000000u | (X * 16u) << 16 | (Y * 16u) << 8 | 0xA0u) |
                                   (X == 0 || Y == 0 || X == 15 || Y == 15 ? 0x00FFFFFFu : 0);
    const int32_t Places[3][3] = { { 40, 480, UNIT }, { 100, 480, 4 * UNIT }, { 290, 470, 3 * UNIT } };
    for (uint32_t K = 0; K < 3u; K++) {
        const BoxBrushMatrix S =
            BoxBrushPlaced(Places[K][0] * ONE, Places[K][1] * ONE, Places[K][2], K == 2 ? BOX_BRUSH_TURN / 12u : 0);
        BoxBrushPaint P = { BOX_BRUSH_PAINT_PICTURE, 0, NULL, 0, S, Picture, 16, 16, 16 };
        O               = Begin(&S);
        BoxBrushAddRect(&O, 0, 0, 16 * ONE, 16 * ONE, 0);
        BoxBrushFill(&B.Canvas, &O, &P, 255);
    }

    /* Pixels one by one. */
    for (int32_t K = 0; K < 200; K++)
        BoxBrushPixel(&B.Canvas, 540 + K, 560 + (int32_t)((BoxBrushSine((uint32_t)K * 900u) * 30) >> 16),
                      0xFFFFFFFFu, 255);

    Words(&B, 0, 540, 620, "imprint 7x16: The quick brown fox — съешь же ещё этих мягких булок ✓", 0xFFE0E0E0u);
    Words(&B, 1, 540, 650, "imprint 11x24: BoxOS ✓ ← → ↑ ↓", 0xFFFFD080u);
    Words(&B, 2, 540, 690, "imprint 15x32: Duppo", 0xFF80D0FFu);
    Words(&B, 1, 540, 740, "translucent text over the sheet", 0x80FFFFFFu);

    const int Written = WritePng(Path, &B.Canvas);
    BoardFree(&B);
    return Written;
}

/* ---------------------------------------------------------------------------
 * Scenes
 * ------------------------------------------------------------------------- */

/* A scene played onto a board of its own, every room the player asks for. */
typedef struct {
    Board          Board;
    BoxScenePlayer Player;
    BoxScenePose  *Start, *Now;
    BoxSceneLaid  *Laid, *Shown;
    BoxBrushEdge  *Edges;
} Stage;

static void StageMake(Stage *T, const BoxScene *S, uint32_t Width, uint32_t Height)
{
    const uint32_t Figures = S->Head->Figures;
    BoardMake(&T->Board, Width, Height);
    T->Start = calloc(Figures, sizeof(BoxScenePose));
    T->Now   = calloc(Figures, sizeof(BoxScenePose));
    T->Laid  = calloc(Figures, sizeof(BoxSceneLaid));
    T->Shown = calloc(Figures, sizeof(BoxSceneLaid));
    T->Edges = calloc(BOX_SCENE_EDGES, sizeof(BoxBrushEdge));
    if (!T->Start || !T->Now || !T->Laid || !T->Shown || !T->Edges) exit(2);
    BoxScenePlayBegin(&T->Player, S, &T->Board.Canvas, T->Start, T->Now, T->Laid, T->Shown, T->Edges);
}

static void StageFree(Stage *T)
{
    BoardFree(&T->Board);
    free(T->Start);
    free(T->Now);
    free(T->Laid);
    free(T->Shown);
    free(T->Edges);
}

/* The frame Ms into the act drawn where it changed; the pixels drawn. */
static uint64_t Frame(Stage *T, uint32_t Ms)
{
    BoxBrushRect   Dirty[8];
    const uint32_t Count = BoxScenePlayAt(&T->Player, Ms, Dirty, 8);
    uint64_t       Drawn = 0;
    BoxScenePlayDraw(&T->Player, Dirty, Count);
    for (uint32_t I = 0; I < Count; I++) Drawn += (uint64_t)Dirty[I].Width * Dirty[I].Height;
    return Drawn;
}

/* The same frame drawn whole by a player of its own, begun from where T's
 * act began: whether it is T's canvas, pixel for pixel. */
static int SameAsWhole(Stage *T, const BoxScene *S, uint32_t Ms)
{
    Stage Whole;
    StageMake(&Whole, S, T->Board.Canvas.Width, T->Board.Canvas.Height);
    memcpy(Whole.Start, T->Start, S->Head->Figures * sizeof(BoxScenePose));
    Whole.Player.Act = T->Player.Act;
    Frame(&Whole, Ms);
    int Same = 1;
    for (uint32_t Y = 0; Y < T->Board.Canvas.Height && Same; Y++)
        if (memcmp(At(&T->Board, 0, (int32_t)Y), At(&Whole.Board, 0, (int32_t)Y), T->Board.Canvas.Width * 4u) != 0)
            Same = 0;
    StageFree(&Whole);
    return Same;
}

static uint8_t *ReadFile(const char *Path, uint64_t *Size)
{
    FILE *In = fopen(Path, "rb");
    if (!In) return NULL;
    fseek(In, 0, SEEK_END);
    *Size          = (uint64_t)ftell(In);
    uint8_t *Bytes = malloc(*Size ? *Size : 1);
    fseek(In, 0, SEEK_SET);
    if (!Bytes || fread(Bytes, 1, *Size, In) != *Size) {
        fclose(In);
        free(Bytes);
        return NULL;
    }
    fclose(In);
    return Bytes;
}

/* The first frame on six screens: the corners the paper, something drawn,
 * and what is drawn the same picture on each — its box, measured in the
 * shorter side, where it is on the others to a pixel or two. */
static void CheckFirstFrames(const BoxScene *S)
{
    static const uint32_t Screens[6][2] = { { 640, 480 }, { 1024, 768 }, { 1280, 800 },
                                            { 1920, 1080 }, { 3840, 2160 }, { 768, 1024 } };
    int    Held = 1;
    double Seen[6][4];
    char   Line[240];
    for (uint32_t I = 0; I < 6u; I++) {
        const uint32_t W = Screens[I][0], H = Screens[I][1], Short = W < H ? W : H;
        const uint32_t Paper = S->Head->Paper;
        Stage          T;
        StageMake(&T, S, W, H);
        Frame(&T, 0);
        int32_t Left = (int32_t)W, Top = (int32_t)H, Right = -1, Bottom = -1;
        for (int32_t Y = 0; Y < (int32_t)H; Y++)
            for (int32_t X = 0; X < (int32_t)W; X++)
                if (*At(&T.Board, X, Y) != Paper) {
                    if (X < Left) Left = X;
                    if (X > Right) Right = X;
                    if (Y < Top) Top = Y;
                    if (Y > Bottom) Bottom = Y;
                }
        if (*At(&T.Board, 0, 0) != Paper || *At(&T.Board, (int32_t)W - 1, (int32_t)H - 1) != Paper || Right < 0 ||
            !Untouched(&T.Board, T.Board.Canvas.Clip))
            Held = 0;
        Seen[I][0] = (Left - W / 2.0) / Short;
        Seen[I][1] = (Right + 1 - W / 2.0) / Short;
        Seen[I][2] = (Top - H / 2.0) / Short;
        Seen[I][3] = (Bottom + 1 - H / 2.0) / Short;
        for (uint32_t K = 0; K < 4u && I; K++)
            if (fabs(Seen[I][K] - Seen[0][K]) > 2.5 / 480.0 + 2.0 / Short) Held = 0;
        StageFree(&T);
    }
    snprintf(Line, sizeof(Line),
             "the first frame on six screens, 640x480 to 3840x2160 and one standing up: the corners the paper, the "
             "same picture on each (%.3f..%.3f across, %.3f..%.3f down, in the shorter side)",
             Seen[0][0], Seen[0][1], Seen[0][2], Seen[0][3]);
    Check(Held, Line);
}

/* Every figure's outline on a screen of 8K, through every frame of enter and
 * a round of wait: none spills past the edges a figure is drawn with. */
static void CheckRoom(const BoxScene *S)
{
    Stage    T;
    int      Held = 1;
    uint32_t Spilled = 0, Looked = 0;
    char     Line[200];
    StageMake(&T, S, 7680, 4320);
    for (uint32_t Act = 0; Act < 2u; Act++) {
        if (Act) {
            BoxBrushRect Ignored[8];
            (void)BoxScenePlayAt(&T.Player, S->Head->Acts[0], Ignored, 8);
            BoxScenePlayAct(&T.Player, Act);
        }
        for (uint32_t Ms = 0; Ms <= S->Head->Acts[Act]; Ms += 100u) {
            BoxBrushRect Ignored[8];
            (void)BoxScenePlayAt(&T.Player, Ms, Ignored, 8);
            for (uint32_t I = 0; I < S->Head->Figures; I++) {
                if (S->Figures[I].Kind == BOX_SCENE_GROUP || T.Laid[I].Opacity == 0) continue;
                BoxBrushOutline O;
                BoxSceneOutline(&T.Player, I, &O);
                Looked++;
                if (O.Spilled) Spilled++;
            }
        }
    }
    Held = Spilled == 0;
    StageFree(&T);
    snprintf(Line, sizeof(Line),
             "on a screen of 8K, every figure seen through enter and a round of wait is drawn whole: %u outline(s), "
             "%u spilled", Looked, Spilled);
    Check(Held, Line);
}

static void CheckPlay(const BoxScene *S, const char *Out)
{
    Stage    T;
    char     Line[300], Path[512];
    int      Held = 1;
    uint64_t Drawn = 0, Frames = 0;
    const uint32_t W = 1024, H = 768;
    StageMake(&T, S, W, H);

    const uint32_t Enter = S->Head->Acts[BOX_SCENE_ENTER], Wait = S->Head->Acts[BOX_SCENE_WAIT];
    const uint32_t Leave = S->Head->Acts[BOX_SCENE_LEAVE];
    Frame(&T, 0);
    snprintf(Path, sizeof(Path), "%s/enter-0000.png", Out);
    WritePng(Path, &T.Board.Canvas);
    for (uint32_t Ms = 33; Ms <= Enter + 33u; Ms += 33u) {
        Drawn += Frame(&T, Ms);
        Frames++;
        if (!SameAsWhole(&T, S, Ms)) Held = 0;
        if (Ms == 495u) {
            snprintf(Path, sizeof(Path), "%s/enter-0495.png", Out);
            WritePng(Path, &T.Board.Canvas);
        }
    }
    Check(Held, "every frame of enter drawn only where it changed is the frame drawn whole, pixel for pixel");

    /* Enter's last frame, and wait's first. */
    Frame(&T, Enter);
    uint32_t *Last = malloc((size_t)W * H * 4u);
    for (uint32_t Y = 0; Y < H; Y++) memcpy(Last + (size_t)Y * W, At(&T.Board, 0, (int32_t)Y), W * 4u);
    BoxScenePlayAct(&T.Player, BOX_SCENE_WAIT);
    Frame(&T, 0);
    int Seam = 1;
    for (uint32_t Y = 0; Y < H; Y++)
        if (memcmp(Last + (size_t)Y * W, At(&T.Board, 0, (int32_t)Y), W * 4u) != 0) Seam = 0;
    free(Last);
    Check(Seam, "wait begins on the very frame enter ended on");

    Held = 1;
    const uint32_t Cut = Wait + Wait / 2u - 55u;
    for (uint32_t Ms = 33; Ms <= Cut; Ms += 33u) {
        Drawn += Frame(&T, Ms);
        Frames++;
        if (!SameAsWhole(&T, S, Ms)) Held = 0;
        if (Ms == 1188u || Ms == 3102u) {
            snprintf(Path, sizeof(Path), "%s/wait-%04u.png", Out, Ms);
            WritePng(Path, &T.Board.Canvas);
        }
    }
    snprintf(Line, sizeof(Line),
             "every frame of a round and a half of wait drawn only where it changed is the frame drawn whole (%llu "
             "frames, %.1f%% of the screen drawn a frame)",
             (unsigned long long)Frames, 100.0 * Drawn / Frames / (W * H));
    Check(Held, Line);

    Held = 1;
    Frame(&T, Cut);
    BoxScenePlayAct(&T.Player, BOX_SCENE_LEAVE);
    for (uint32_t Ms = 0; Ms <= Leave + 33u; Ms += 33u) {
        Frame(&T, Ms);
        if (!SameAsWhole(&T, S, Ms)) Held = 0;
        if (Ms == 231u) {
            snprintf(Path, sizeof(Path), "%s/leave-0231.png", Out);
            WritePng(Path, &T.Board.Canvas);
        }
    }
    int            Even = 1;
    const uint32_t Ends = *At(&T.Board, 0, 0);
    for (uint32_t Y = 0; Y < H; Y++)
        for (uint32_t X = 0; X < W; X++)
            if (*At(&T.Board, (int32_t)X, (int32_t)Y) != Ends) Even = 0;
    snprintf(Line, sizeof(Line),
             "leave, begun in the middle of a round, drawn where it changed is the frame drawn whole, and ends in one "
             "colour, #%06X, nothing of the scene left", Ends & 0xFFFFFFu);
    Check(Held && Even && Untouched(&T.Board, T.Board.Canvas.Clip), Line);
    StageFree(&T);
}

/* A copy of the scene with four bytes at Offset written as Value — none when
 * Offset lies past it — read as Cut bytes, or all of it. */
/* The scene with Span words from Offset set to Value, read Cut bytes long (0:
 * whole): why it is refused, or NULL. */
static const char *Damaged(const uint8_t *Bytes, uint64_t Size, uint64_t Offset, uint32_t Value, uint32_t Span,
                           uint64_t Cut)
{
    static uint8_t Copy[BOX_SCENE_FILE_MOST];
    BoxScene       S;
    if (Size > sizeof(Copy)) return "the stand's room is too small";
    memcpy(Copy, Bytes, Size);
    for (uint32_t K = 0; K < Span; K++)
        if (Size >= 4u && Offset + 4u * K <= Size - 4u) memcpy(Copy + Offset + 4u * K, &Value, 4);
    return BoxSceneRead(Copy, Cut ? Cut : Size, &S);
}

static void CheckRefusals(const uint8_t *Bytes, uint64_t Size, const BoxScene *S)
{
    const BoxSceneHead *H       = S->Head;
    const uint64_t      Paints  = sizeof(BoxSceneHead);
    const uint64_t      Stops   = Paints + (uint64_t)H->Paints * sizeof(BoxScenePaint);
    const uint64_t      Figures = Stops + (uint64_t)H->Stops * sizeof(BoxBrushStop);
    const uint64_t      Pieces  = Figures + (uint64_t)H->Figures * sizeof(BoxSceneFigure);
    const uint64_t      Tracks  = Pieces + (uint64_t)H->Pieces * sizeof(BoxScenePiece);
    const uint64_t      Keys    = Tracks + (uint64_t)H->Tracks * sizeof(BoxSceneTrack);
    /* The first figure that is a shape of its own, and whether there are
     * tracks and keys to damage: the cases are the scene's, whatever it holds. */
    uint32_t Shape = 0;
    while (Shape < H->Figures && S->Figures[Shape].Kind != BOX_SCENE_ELLIPSE && S->Figures[Shape].Kind != BOX_SCENE_RECT)
        Shape++;
    const uint64_t Own = Figures + (uint64_t)Shape * sizeof(BoxSceneFigure);
    typedef struct {
        uint64_t    Offset;
        uint32_t    Value;
        uint32_t    Span;
        uint64_t    Cut;
        int         Asks;   /* what the scene must hold for the case: 0 nothing, 1 a shape, 2 two tracks */
        const char *Why;
    } Case;
    const Case Cases[] = {
        { 0, 0x584F4258u, 1, 0, 0, "it does not begin as a scene does" },
        { ~0ull, 0, 1, Size - 1u, 0, "its size is not the size of its tables" },
        { 8, (uint32_t)Size + 12u, 1, 0, 0, "its size is not the size of its tables" },
        { 12, 3, 1, 0, 0, "its units are past what a screen holds" },
        { 16, 0x01000000u, 1, 0, 0, "its paper is no colour" },
        { 24, 0, 1, 0, 0, "its wait act has no length to go round" },
        { Paints, 9, 1, 0, 0, "a paint of no kind a scene has" },
        { Stops, BOX_BRUSH_UNIT + 1u, 1, 0, 0, "a gradient's stops do not rise from 0 to 1" },
        { Figures + 4u, 5, 1, 0, 0, "a figure's group is not a group laid before it" },
        { Figures + sizeof(BoxSceneFigure) + 4u, 1, 1, 0, 0, "a figure's group is not a group laid before it" },
        { Own + 8u, 0xFFFFu, 1, 0, 1, "a figure is painted with no paint the scene keeps" },
        { Own + 20u, 0, 1, 0, 1, "a figure's shape has no size" },
        { Figures + sizeof(BoxSceneFigure) + 56u, 256, 1, 0, 0, "a figure stands where none may" },
        { Tracks + 4u, 0xFFFFu, 1, 0, 2, "a track moves what the scene has not" },
        { Tracks + sizeof(BoxSceneTrack), 0, 3, 0, 2, "the tracks are not kept by act, figure and what, one for each" },
        { Tracks + 16u, 0x7FFFFFFFu, 1, 0, 2, "a track's keys lie past the keys kept" },
        { Keys + sizeof(BoxSceneKey), 999999, 1, 0, 2, "a track's keys do not rise within its act" },
        { Keys + 8u, 7, 1, 0, 2, "a track's key is none a scene has" },
    };
    int    Held = 1;
    size_t Tried = 0;
    char   Line[200];
    for (size_t I = 0; I < sizeof(Cases) / sizeof(Cases[0]); I++) {
        if ((Cases[I].Asks == 1 && Shape == H->Figures) || (Cases[I].Asks == 2 && H->Tracks < 2u)) continue;
        const char *Why = Damaged(Bytes, Size, Cases[I].Offset, Cases[I].Value, Cases[I].Span, Cases[I].Cut);
        Tried++;
        if (!Why || strcmp(Why, Cases[I].Why) != 0) {
            printf("    case %zu: wanted \"%s\", got \"%s\"\n", I, Cases[I].Why, Why ? Why : "(read)");
            Held = 0;
        }
    }
    snprintf(Line, sizeof(Line), "the scene damaged %zu named ways, one at a time: each refused for what is wrong",
             Tried);
    Check(Held, Line);

    /* A path's pieces damaged, when the scene draws one. */
    uint32_t Path = 0, Cubic = 0;
    while (Path < H->Figures && S->Figures[Path].Kind != BOX_SCENE_PATH) Path++;
    while (Cubic < H->Pieces && S->Pieces[Cubic].Verb != BOX_SCENE_CUBIC) Cubic++;
    if (Path < H->Figures && Cubic < H->Pieces) {
        const uint64_t Figure = Figures + (uint64_t)Path * sizeof(BoxSceneFigure);
        const uint64_t First  = Pieces + (uint64_t)S->Figures[Path].Shape[0] * sizeof(BoxScenePiece);
        const struct {
            uint64_t    Offset;
            uint32_t    Value;
            const char *Why;
        } Paths[] = {
            { Figure + 16u, H->Pieces + 1u, "a path's pieces lie past the pieces kept" },
            { Figure + 16u, 1, "a path's pieces lie past the pieces kept" },
            { Figure + 20u, 1, "a path's pieces lie past the pieces kept" },
            { First, 9, "a path's pieces are none the pen follows" },
            { First, BOX_SCENE_LINE, "a path's pieces are none the pen follows" },
            { First + 4u, 0x7FFFFFFFu, "a path reaches past what the brush draws" },
            { Pieces + (uint64_t)Cubic * sizeof(BoxScenePiece), BOX_SCENE_LINE, "a path's curve is cut short" },
        };
        Held = 1;
        for (size_t I = 0; I < sizeof(Paths) / sizeof(Paths[0]); I++) {
            const char *Why = Damaged(Bytes, Size, Paths[I].Offset, Paths[I].Value, 1, 0);
            if (!Why || strcmp(Why, Paths[I].Why) != 0) {
                printf("    path case %zu: wanted \"%s\", got \"%s\"\n", I, Paths[I].Why, Why ? Why : "(read)");
                Held = 0;
            }
        }
        snprintf(Line, sizeof(Line), "a path damaged %zu named ways, one at a time: each refused for what is wrong",
                 sizeof(Paths) / sizeof(Paths[0]));
        Check(Held, Line);
    }

    /* Random damage: read, and what reads played, all under the sanitizers. */
    static uint8_t Copy[BOX_SCENE_FILE_MOST];
    uint32_t       Read = 0;
    Held                = 1;
    for (uint32_t I = 0; I < 20000u; I++) {
        memcpy(Copy, Bytes, Size);
        const uint32_t Flips = 1u + Random() % 8u;
        for (uint32_t K = 0; K < Flips; K++) Copy[Random() % Size] ^= (uint8_t)(1u << (Random() % 8u));
        const uint64_t Cut = I % 10u == 0 ? Random() % Size : Size;
        uint8_t       *Held_ = malloc(Cut ? Cut : 1);
        memcpy(Held_, Copy, Cut);
        BoxScene Damage;
        if (!BoxSceneRead(Held_, Cut, &Damage)) {
            Read++;
            Stage T;
            StageMake(&T, &Damage, 96, 72);
            for (uint32_t Act = 0; Act < BOX_SCENE_ACTS; Act++) {
                if (Act) BoxScenePlayAct(&T.Player, Act);
                for (uint32_t Ms = 0; Ms < 3000u; Ms += 450u) Frame(&T, Ms);
            }
            if (!Untouched(&T.Board, T.Board.Canvas.Clip)) Held = 0;
            StageFree(&T);
        }
        free(Held_);
    }
    snprintf(Line, sizeof(Line),
             "20000 random damages read without a byte past the scene; the %u that read played through every act "
             "with no pixel past the canvas", Read);
    Check(Held, Line);
}

static int Frames(const char *Path, const char *Out)
{
    uint64_t Size  = 0;
    uint8_t *Bytes = ReadFile(Path, &Size);
    BoxScene S;
    if (!Bytes) {
        Check(0, "the scene could not be read from its file");
        return 0;
    }
    const char *Why = BoxSceneRead(Bytes, Size, &S);
    char        Line[200];
    snprintf(Line, sizeof(Line), "BoxSceneRead holds the scene to every bound: %s (%u figures, %u tracks)",
             Why ? Why : "read", S.Head->Figures, S.Head->Tracks);
    Check(!Why, Line);
    if (Why) return 0;
    CheckFirstFrames(&S);
    CheckRoom(&S);
    CheckPlay(&S, Out);
    CheckRefusals(Bytes, Size, &S);
    free(Bytes);
    return 1;
}

/* The scene's first frame on a Width x Height screen, written as a PPM. */
static int First(const char *Path, uint32_t Width, uint32_t Height, const char *Out)
{
    uint64_t Size  = 0;
    uint8_t *Bytes = ReadFile(Path, &Size);
    BoxScene S;
    if (!Bytes || BoxSceneRead(Bytes, Size, &S) || !Width || !Height || Width > 8192u || Height > 8192u) {
        fprintf(stderr, "scene_stand: %s is no scene to draw at %ux%u\n", Path, Width, Height);
        free(Bytes);
        return 2;
    }
    Stage T;
    StageMake(&T, &S, Width, Height);
    Frame(&T, 0);
    FILE *File = fopen(Out, "wb");
    if (!File) return 2;
    fprintf(File, "P6\n%u %u\n255\n", Width, Height);
    for (uint32_t Y = 0; Y < Height; Y++)
        for (uint32_t X = 0; X < Width; X++) {
            const uint32_t P      = *At(&T.Board, (int32_t)X, (int32_t)Y);
            const uint8_t  Rgb[3] = { (uint8_t)(P >> 16), (uint8_t)(P >> 8), (uint8_t)P };
            fwrite(Rgb, 1, 3, File);
        }
    const int Written = fclose(File) == 0;
    StageFree(&T);
    free(Bytes);
    return Written ? 0 : 2;
}

int main(int Argc, char **Argv)
{
    if (Argc == 6 && strcmp(Argv[1], "first") == 0)
        return First(Argv[2], (uint32_t)strtoul(Argv[3], NULL, 10), (uint32_t)strtoul(Argv[4], NULL, 10), Argv[5]);
    if (Argc == 4 && strcmp(Argv[1], "frames") == 0) {
        Frames(Argv[2], Argv[3]);
        printf("scene: %d passed, %d failed — the scene %s\n", g_passed, g_failed, Argv[2]);
        return g_failed ? 1 : 0;
    }
    if (Argc != 3 || strcmp(Argv[1], "brush") != 0) {
        fprintf(stderr, "usage: scene_stand brush SHEET.png | frames SCENE OUTDIR | first SCENE WIDTH HEIGHT OUT.ppm\n");
        return 2;
    }
    CheckNumbers();
    CheckPress();
    CheckRects();
    CheckRound();
    CheckLinesCurves();
    CheckTurns();
    CheckPaints();
    CheckClip();
    CheckRefused();
    Check(DrawSheet(Argv[2]), "the sheet of everything the brush draws is written");
    printf("scene: %d passed, %d failed — the brush\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
