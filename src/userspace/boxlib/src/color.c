
#include "box/color.h"

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool color_parse(const char *text, Color *out)
{
    if (!text || !out) return false;
    if (*text == '#') text++;

    uint32_t value = 0;
    for (int i = 0; i < 6; i++) {
        int d = hex_digit(text[i]);
        if (d < 0) return false;
        value = (value << 4) | (uint32_t)d;
    }
    if (text[6] != '\0') return false;

    *out = (Color)value;
    return true;
}

void color_format(Color c, char out[8])
{
    static const char kHex[] = "0123456789abcdef";
    if (!out) return;
    out[0] = '#';
    for (int i = 0; i < 6; i++) {
        uint32_t nibble = (c >> (20 - i * 4)) & 0xFu;
        out[1 + i] = kHex[nibble];
    }
    out[7] = '\0';
}

Color color_wheel(uint16_t hue, uint8_t sat, uint8_t val)
{
    uint32_t h = (uint32_t)hue % 1536u;
    uint32_t sector = h / 256u;
    uint32_t within = h % 256u;

    uint32_t v = val;
    uint32_t p = (v * (255u - sat)) / 255u;
    uint32_t rising  = p + ((v - p) * within) / 255u;
    uint32_t falling = v - ((v - p) * within) / 255u;

    uint32_t r, g, b;
    switch (sector) {
    case 0:  r = v;       g = rising;  b = p;       break;
    case 1:  r = falling; g = v;       b = p;       break;
    case 2:  r = p;       g = v;       b = rising;  break;
    case 3:  r = p;       g = falling; b = v;       break;
    case 4:  r = rising;  g = p;       b = v;       break;
    default: r = v;       g = p;       b = falling; break;
    }
    return COLOR_RGB(r, g, b);
}

Color color_mix(Color a, Color b, uint8_t weight)
{
    uint32_t w  = weight;
    uint32_t iw = 255u - w;
    uint32_t r = (COLOR_R(a) * iw + COLOR_R(b) * w) / 255u;
    uint32_t g = (COLOR_G(a) * iw + COLOR_G(b) * w) / 255u;
    uint32_t bl = (COLOR_B(a) * iw + COLOR_B(b) * w) / 255u;
    return COLOR_RGB(r, g, bl);
}