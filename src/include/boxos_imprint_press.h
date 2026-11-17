/*
 * boxos_imprint_press.h — the imprint (boxos_imprint.h) pressed into a
 * framebuffer: a glyph's steps as the pixels of its cell, ink laid over paper
 * as every colour is (BoxColorOver), and the size of the cells when nobody has
 * chosen one.
 *
 * The hands that draw from the imprint with nothing under them — the kernel's
 * Emergency Screen and Gangway — press a character through here, so it lies on
 * the glass the one way from the loader on. No memory is asked for, no float
 * is used: it works in a panic as anywhere.
 *
 * The includer supplies uint8_t, uint16_t, uint32_t, int32_t and the uint64_t
 * boxos_screen.h takes.
 */
#ifndef BOXOS_IMPRINT_PRESS_H
#define BOXOS_IMPRINT_PRESS_H

#include "boxos_color.h"
#include "boxos_imprint.h"
#include "boxos_pixel.h"
#include "boxos_screen.h"

/* The pixel each of a glyph's steps is pressed as — the paper, the mixes
 * between, the ink — in the framebuffer's layout Format (BOX_PIXEL_*). */
static inline void BoxPressInks(uint32_t Paper, uint32_t Ink, uint32_t Format, uint32_t Inks[BOXOS_IMPRINT_INKED + 1u])
{
    for (uint32_t Share = 0; Share <= BOXOS_IMPRINT_INKED; Share++)
        Inks[Share] = BoxPixelEncode(Share == 0                     ? Paper
                                     : Share == BOXOS_IMPRINT_INKED ? Ink
                                                                    : BoxColorOver(Paper, Ink, Share * 255u / BOXOS_IMPRINT_INKED),
                                     Format);
}

/* Glyph Glyph of size Size pressed into the cell whose top-left pixel is At,
 * Stride bytes from one row of the framebuffer to the next, four a pixel. */
static inline void BoxPressGlyph(const BoxImprint *Imprint, uint32_t Size, uint32_t Glyph, volatile uint8_t *At,
                                 uint32_t Stride, const uint32_t Inks[BOXOS_IMPRINT_INKED + 1u])
{
    const BoxImprintSize *Cell = &Imprint->Sizes[Size];
    for (uint32_t Y = 0; Y < Cell->Height; Y++, At += Stride) {
        volatile uint32_t *Pixel = (volatile uint32_t *)At;
        for (uint32_t X = 0; X < Cell->Width; X++) Pixel[X] = Inks[BoxImprintShade(Imprint, Size, Glyph, X, Y)];
    }
}

/* The size a glass Height pixels tall is drawn in when nobody has chosen one:
 * the cell that shows about SCREEN_ROWS_CHOSEN rows. */
static inline uint32_t BoxPressSize(const BoxImprint *Imprint, uint32_t Height)
{
    return BoxImprintSizeFor(Imprint, (Height + SCREEN_ROWS_CHOSEN / 2u) / SCREEN_ROWS_CHOSEN);
}

#endif /* BOXOS_IMPRINT_PRESS_H */
