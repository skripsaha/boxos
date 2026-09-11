#ifndef BOX_COLOR_H
#define BOX_COLOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "boxos_color.h"


typedef uint32_t Color;


#define COLOR_BLACK         COLOR_RGB(0x00, 0x00, 0x00)
#define COLOR_WHITE         COLOR_RGB(0xFF, 0xFF, 0xFF)

#define COLOR_DARK_GRAY     COLOR_RGB(0x55, 0x55, 0x55)
#define COLOR_LIGHT_GRAY    COLOR_RGB(0xAA, 0xAA, 0xAA)

#define COLOR_RED           COLOR_RGB(0xE0, 0x40, 0x40)
#define COLOR_GREEN         COLOR_RGB(0x40, 0xC0, 0x40)
#define COLOR_BLUE          COLOR_RGB(0x40, 0x80, 0xE0)
#define COLOR_YELLOW        COLOR_RGB(0xE0, 0xC0, 0x40)
#define COLOR_CYAN          COLOR_RGB(0x40, 0xC0, 0xC0)
#define COLOR_MAGENTA       COLOR_RGB(0xC0, 0x40, 0xC0)

#define COLOR_DARK_RED      COLOR_RGB(0xA0, 0x00, 0x00)
#define COLOR_DARK_GREEN    COLOR_RGB(0x00, 0x80, 0x00)
#define COLOR_DARK_BLUE     COLOR_RGB(0x00, 0x00, 0xA0)
#define COLOR_BROWN         COLOR_RGB(0xA0, 0x60, 0x00)
#define COLOR_DARK_CYAN     COLOR_RGB(0x00, 0x80, 0x80)
#define COLOR_DARK_MAGENTA  COLOR_RGB(0x80, 0x00, 0x80)

#define COLOR_BERRY         COLOR_RGB(0xE0, 0x4F, 0x90)
#define COLOR_OCEAN         COLOR_RGB(0x10, 0x80, 0xC0)
#define COLOR_LEAF          COLOR_RGB(0x60, 0xC0, 0x30)
#define COLOR_AMBER         COLOR_RGB(0xFF, 0xB0, 0x40)
#define COLOR_VIOLET        COLOR_RGB(0x90, 0x60, 0xFF)
#define COLOR_TEAL          COLOR_RGB(0x00, 0xB0, 0xA0)
#define COLOR_CORAL         COLOR_RGB(0xFF, 0x80, 0x70)
#define COLOR_SLATE         COLOR_RGB(0x60, 0x70, 0x80)


bool  color_parse(const char *text, Color *out);

void  color_format(Color c, char out[8]);

Color color_wheel(uint16_t hue, uint8_t sat, uint8_t val);

Color color_mix(Color a, Color b, uint8_t weight);


void  set_color(Color fg);
Color get_color(void);
void  set_color_bg(Color bg);
Color get_color_bg(void);

#ifdef __cplusplus
}
#endif

#endif