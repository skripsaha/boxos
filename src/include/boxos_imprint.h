/*
 * boxos_imprint.h — the imprint, and how a hand reads it.
 *
 * The imprint is the system's characters drawn ahead of time — by Typecase,
 * from the system's font, its geometry and BoxOS's own symbols — at a few
 * cell sizes, for the hands that cannot read a font: the kernel's Emergency
 * Screen, the display's rescue when its font will not read, the loader. Such
 * a hand reads no TTF, works out no curve and asks for no memory: it finds a
 * character and takes its pixels, so it draws in a panic as well as anywhere.
 *
 * tools/imprint makes it from the font at every build (boxos_imprint_data.h
 * in the build's own directory, not kept in the tree), so the font is the one
 * account of how text looks, here as on the display.
 *
 * Its characters are sorted by code. At each size — the sizes from the
 * smallest cell to the largest — every character has a glyph of the whole
 * cell, and after the last of them one more: the empty box a character the
 * imprint lacks is drawn as. A glyph is its cell's pixels row by row, two
 * bits each, the lowest first in a byte — how much of the pixel the
 * character inks, 0 none to 3 all — and begins on a byte of its own.
 *
 * The includer supplies uint8_t and uint32_t.
 */
#ifndef BOXOS_IMPRINT_H
#define BOXOS_IMPRINT_H

typedef struct BoxImprintSize {
    uint8_t  Width;      /* the cell, in pixels */
    uint8_t  Height;
    uint8_t  Baseline;   /* rows from the cell's top down to the baseline */
    uint8_t  Reserved;
    uint32_t Offset;     /* where this size's first glyph begins in the pixels */
} BoxImprintSize;

typedef struct BoxImprint {
    const uint32_t       *Characters;   /* sorted by code */
    uint32_t              Count;
    const BoxImprintSize *Sizes;        /* smallest cell first */
    uint32_t              SizeCount;
    const uint8_t        *Pixels;
} BoxImprint;

/* How much a pixel's two bits say it is inked, out of BOXOS_IMPRINT_INKED. */
#define BOXOS_IMPRINT_INKED 3u

/* The glyph of Character: its place among the characters, or Count — the
 * empty box — when the imprint has none. Found by halves. */
static inline uint32_t BoxImprintFind(const BoxImprint *Imprint, uint32_t Character)
{
    uint32_t Low = 0, High = Imprint->Count;
    while (Low < High) {
        const uint32_t Middle = Low + (High - Low) / 2;
        if (Imprint->Characters[Middle] < Character) Low = Middle + 1;
        else if (Imprint->Characters[Middle] > Character) High = Middle;
        else return Middle;
    }
    return Imprint->Count;
}

/* The largest size whose cell is no taller than Height; the smallest when
 * even that is taller. */
static inline uint32_t BoxImprintSizeFor(const BoxImprint *Imprint, uint32_t Height)
{
    uint32_t Chosen = 0;
    for (uint32_t Size = 1; Size < Imprint->SizeCount; Size++)
        if (Imprint->Sizes[Size].Height <= Height) Chosen = Size;
    return Chosen;
}

/* How much of pixel X, Y of glyph Glyph at size Size is inked, 0 to
 * BOXOS_IMPRINT_INKED. */
static inline uint32_t BoxImprintShade(const BoxImprint *Imprint, uint32_t Size, uint32_t Glyph, uint32_t X, uint32_t Y)
{
    const BoxImprintSize *Of    = &Imprint->Sizes[Size];
    const uint32_t        Bytes = ((uint32_t)Of->Width * Of->Height * 2u + 7u) / 8u;
    const uint32_t        Pixel = Y * Of->Width + X;
    const uint8_t         Four  = Imprint->Pixels[Of->Offset + Glyph * Bytes + Pixel / 4u];
    return (Four >> ((Pixel % 4u) * 2u)) & 3u;
}

#endif /* BOXOS_IMPRINT_H */
