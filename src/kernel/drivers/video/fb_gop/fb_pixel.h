#ifndef FB_PIXEL_H
#define FB_PIXEL_H

#include "ktypes.h"

#define FB_FORMAT_RGB   0u
#define FB_FORMAT_BGR   1u
#define FB_FORMAT_BGRX  2u

static inline uint32_t FbPixelEncode(uint32_t rgb, uint32_t format)
{
    if (format == FB_FORMAT_RGB) {
        uint8_t r = (rgb >> 16) & 0xFF;
        uint8_t g = (rgb >>  8) & 0xFF;
        uint8_t b =  rgb        & 0xFF;
        return ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
    }
    return rgb;
}

#endif