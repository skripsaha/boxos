#ifndef MANDELBROT_H
#define MANDELBROT_H

/* mandelbrot — the set and its Julia sets, to look into as deep as a double
 * reaches. mandelbrot_paint.cpp works out every pixel and colours it, and
 * takes nothing of BoxOS; mandelbrot.cpp is the view, the keys and the glass.
 *
 * A pixel is worked out once into how far it got before it escaped (a smooth
 * count, so colours do not band) and kept; colours are laid on those counts,
 * so a new palette or the colours turning costs nothing more than laying them
 * again. The picture comes coarse first and grows sharp — blocks of eight
 * pixels, then four, two, one — so a move is seen at once. */

#include <cstdint>
#include <vector>

#include "painter/core/color.h"

using PainterColor = painter::core::Color;

/* Where the view looks and how wide it is, in the complex plane. */
struct FractalView {
    double CentreX = -0.6, CentreY = 0.0;
    double Width   = 3.4;
    bool   Julia   = false;
    double JuliaX  = 0.0, JuliaY = 0.0;   /* c, for a Julia set */
};

/* How many steps a pixel is followed before it is called inside: more the
 * deeper the view, times what the person asked for. */
int FractalSteps(const FractalView &View, double Detail);

/* The work of one picture, pass by pass. */
class FractalPicture {
public:
    void Resize(int Wide, int High);
    /* Begun again for a new view. */
    void Restart(const FractalView &View, int Steps);
    /* Work for about `Budget` microseconds; false once the picture is whole. */
    bool Work(std::uint64_t Budget);
    bool Whole() const { return Step == 0; }
    /* The pass under way: 8, 4, 2, 1; 0 when whole. */
    int  Pass() const { return Step; }

    int Width = 0, Height = 0;
    /* Smooth escape counts, row by row; < 0 inside the set. */
    std::vector<float> Counts;

private:
    void Fill(int X, int Y, float Count);

    FractalView View;
    int         Steps = 0;
    int         Step  = 0;   /* block size of the pass under way */
    int         Row   = 0;   /* next row of it */
};

/* A gradient of 256 colours, looped. */
struct FractalPalette {
    const char *Name;
    PainterColor Ramp[256];
};

const FractalPalette &FractalPalettes(int Which);
int                   FractalPaletteCount();

/* The counts coloured: every `Spread` steps of the count go once round the
 * palette, which is turned by `Turn`; inside is black. */
void FractalColour(const FractalPicture &Picture, const FractalPalette &Palette, float Spread, float Turn,
                   std::vector<PainterColor> &Pixels);

#endif
