/*
 * color.c — the colour word, said and read.
 *
 * BoxOS carries colour as a packed #RRGGBB the whole way to the glass, and
 * every header in the tree spells it "#rrggbb" — but until now nothing could
 * read that spelling back. A program that wanted a colour from the person
 * using it had to parse six hex digits itself, and two programs doing that
 * are two chances to disagree about what "#F00" means (here: nothing — the
 * short form is not a spelling this system uses).
 *
 * The wheel and the mix are here for the same reason: a rainbow and a
 * gradient are arithmetic, not libm, and a cabin has no libm. The wheel's
 * scale is the one the VGA projection already uses (six sectors of 256), so
 * a hue that reads as red on a framebuffer reads as red on a text console.
 */

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
    /* Six digits and then the end of the word: "#ff000000" is not a colour
     * with something after it, it is a mistake, and saying so is the whole
     * point of a parser. */
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
    uint32_t sector = h / 256u;          /* 0..5: R Y G C B M */
    uint32_t within = h % 256u;          /* how far into the sector */

    uint32_t v = val;
    uint32_t p = (v * (255u - sat)) / 255u;                       /* the floor  */
    uint32_t rising  = p + ((v - p) * within) / 255u;             /* p -> v     */
    uint32_t falling = v - ((v - p) * within) / 255u;             /* v -> p     */

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
