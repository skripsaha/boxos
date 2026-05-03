#ifndef FB_PIXEL_H
#define FB_PIXEL_H

#include "ktypes.h"

#define FB_FORMAT_RGB   0u
#define FB_FORMAT_BGR   1u
#define FB_FORMAT_BGRX  2u

extern const uint32_t vga_palette[16];

/*
 * vga_palette[] entries follow the standard #RRGGBB convention: R in
 * bits 23..16, G in bits 15..8, B in bits 7..0. When a uint32_t with this
 * layout is written to memory little-endian, byte 0 (LSB) holds B and
 * byte 2 holds R — i.e. the in-memory order is B, G, R, X. That layout
 * matches PixelBlueGreenRedReserved8BitPerColor (BGR/BGRX HW) directly,
 * so for BGR formats we write the palette value AS-IS — no swap.
 *
 * For PixelRedGreenBlueReserved8BitPerColor (RGB HW), byte 0 must hold R.
 * We have to swap R and B in the encoded value so that LE memory order
 * becomes R, G, B, X.
 *
 * Pre-fix this logic was inverted: BGR was swapped (turning red into
 * blue) and RGB was passed through (also wrong). The classic symptom
 * was COLOR_CYAN rendering as yellow and COLOR_RED as blue under UEFI
 * GOP boots that report BGR pixel format (the QEMU/OVMF default).
 */
static inline uint32_t FbPixelEncode(uint32_t rgb, uint32_t format)
{
    if (format == FB_FORMAT_RGB) {
        uint8_t r = (rgb >> 16) & 0xFF;
        uint8_t g = (rgb >>  8) & 0xFF;
        uint8_t b =  rgb        & 0xFF;
        return ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
    }
    /* FB_FORMAT_BGR or FB_FORMAT_BGRX: palette is already in BGR memory
     * order when written little-endian — pass through unchanged. */
    return rgb;
}

#endif /* FB_PIXEL_H */
