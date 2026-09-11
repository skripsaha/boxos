#ifndef BOXOS_COLOR_H
#define BOXOS_COLOR_H



#define COLOR_RGB(r, g, b)  ((uint32_t)((((uint32_t)(r) & 0xFFu) << 16) | \
                                        (((uint32_t)(g) & 0xFFu) <<  8) | \
                                        ((uint32_t)(b) & 0xFFu)))

#define COLOR_R(c)  (((c) >> 16) & 0xFFu)
#define COLOR_G(c)  (((c) >>  8) & 0xFFu)
#define COLOR_B(c)  ((c) & 0xFFu)

#define COLOR_DEFAULT  ((uint32_t)0xFE000000u)
#define COLOR_INHERIT  ((uint32_t)0xFF000000u)


static inline uint32_t BoxVgaPaletteRgb(uint8_t idx)
{
    static const uint32_t pal[16] = {
        0x000000u,
        0x0000AAu,
        0x00AA00u,
        0x00AAAAu,
        0xAA0000u,
        0xAA00AAu,
        0xAA5500u,
        0xAAAAAAu,
        0x555555u,
        0x5555FFu,
        0x55FF55u,
        0x55FFFFu,
        0xFF5555u,
        0xFF55FFu,
        0xFFFF55u,
        0xFFFFFFu,
    };
    return pal[idx & 0x0Fu];
}


static inline uint32_t BoxColorResolveFg(uint32_t c)
{
    return (c >> 24) ? BoxVgaPaletteRgb(7)  : c;
}

static inline uint32_t BoxColorResolveBg(uint32_t c)
{
    return (c >> 24) ? 0x000000u  : c;
}

static inline uint8_t BoxColorToVga4(uint32_t rgb)
{
    uint32_t r = COLOR_R(rgb);
    uint32_t g = COLOR_G(rgb);
    uint32_t b = COLOR_B(rgb);
    uint32_t M = r > g ? r : g;  if (b > M) M = b;
    uint32_t m = r < g ? r : g;  if (b < m) m = b;
    uint32_t C = M - m;
    uint32_t L = (M + m) / 2;

    if (M == 0 || (C * 255u) / M < 85u) {
        if (L < 43u)  return 0x0;
        if (L < 128u) return 0x8;
        if (L < 213u) return 0x7;
        return 0xF;
    }

    int32_t h;
    if (M == r)      h = (int32_t)(256 * ((int32_t)g - (int32_t)b) / (int32_t)C);
    else if (M == g) h = 512 + (int32_t)(256 * ((int32_t)b - (int32_t)r) / (int32_t)C);
    else             h = 1024 + (int32_t)(256 * ((int32_t)r - (int32_t)g) / (int32_t)C);
    if (h < 0) h += 1536;
    uint32_t sector = (uint32_t)((h + 128) / 256) % 6u;

    static const uint8_t dark_of [6] = { 0x4, 0x6, 0x2, 0x3, 0x1, 0x5 };
    static const uint8_t light_of[6] = { 0xC, 0xE, 0xA, 0xB, 0x9, 0xD };
    return L >= 128u ? light_of[sector] : dark_of[sector];
}


static inline uint8_t BoxColorPairToAttr(uint32_t fg, uint32_t bg)
{
    uint8_t f = BoxColorToVga4(BoxColorResolveFg(fg));
    uint8_t b = BoxColorToVga4(BoxColorResolveBg(bg));
    return (uint8_t)((b << 4) | f);
}

static inline uint32_t BoxAttrFgRgb(uint8_t attr)
{
    return BoxVgaPaletteRgb((uint8_t)(attr & 0x0Fu));
}

static inline uint32_t BoxAttrBgRgb(uint8_t attr)
{
    return BoxVgaPaletteRgb((uint8_t)((attr >> 4) & 0x0Fu));
}

#endif