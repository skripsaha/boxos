#ifndef FB_PIXEL_H
#define FB_PIXEL_H

#include "ktypes.h"

#define FB_FORMAT_RGB   0u
#define FB_FORMAT_BGR   1u
#define FB_FORMAT_BGRX  2u

/*
 * FbPixelEncode — one #RRGGBB value into the device pixel layout.
 *
 * A #RRGGBB uint32_t (R in bits 23..16, G in 15..8, B in 7..0) written to
 * memory little-endian puts B in byte 0 and R in byte 2 — i.e. in-memory
 * order B, G, R, X. That layout matches
 * PixelBlueGreenRedReserved8BitPerColor (BGR/BGRX HW) directly, so for BGR
 * formats the value is written AS-IS — no swap.
 *
 * For PixelRedGreenBlueReserved8BitPerColor (RGB HW), byte 0 must hold R,
 * so R and B swap in the encoded value to make LE memory order R, G, B, X.
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
    /* FB_FORMAT_BGR or FB_FORMAT_BGRX: already in BGR memory order when
     * written little-endian — pass through unchanged. */
    return rgb;
}

#endif /* FB_PIXEL_H */
