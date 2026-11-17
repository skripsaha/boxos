#ifndef BOX_COLOR_H
#define BOX_COLOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "boxos_color.h"

/*
 * Color — 24-bit RGB packed into uint32_t (0x00RRGGBB).
 *
 * BoxOS color model is not ANSI-escape-codes: there are no in-band escape
 * sequences in text streams. Color is metadata, attached to a text segment
 * via a structured op (printf "%color" specifier produces one Manifest op
 * per colored run). One printf call → one syscall regardless of how many
 * color changes occur inside.
 *
 * The full 24-bit value travels the whole path — frame, daemon, kernel op,
 * TextCell. The GOP framebuffer renders it exactly; the VGA text backend
 * quantizes to its 16-color palette at draw time, inside the kernel — the
 * one place that actually has a 4-bit limitation (boxos_color.h holds the
 * shared palette + quantizer).
 *
 * Reserved sentinels (defined in boxos_color.h):
 *   COLOR_DEFAULT   "use the role's default" — the screen's own: fg its text
 *                   colour, bg its page colour, as the display draws them
 *                   (light gray on black until the person chooses others).
 *   COLOR_INHERIT   "keep currently set background" (use as bg only).
 *   COLOR_THROUGH   "see through to what is beneath" — a layer's cell only
 *                   (box/display.h): the display shows, in its place, the
 *                   background of the cell beneath the layer's; both of a
 *                   cell's colours so, and the cell beneath shows unchanged.
 *   0x80..0xBF      top byte of a translucent colour — a layer's cell only: the
 *                   #RRGGBB in the low 24 bits at (byte - 0x80) / 63 opacity
 *                   over the background of the cell beneath, blended by the
 *                   display into a plain triple (boxos_color.h).
 */

typedef uint32_t Color;

/* ------------------------------------------------------------------------- */
/* Standard BoxOS palette — RGB triples chosen to look readable on black bg.
 * Names are descriptive, not POSIX/ANSI: this is BoxOS, not a terminal. */

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

/* BoxOS-flavour accents — unique names rather than ANSI clones. */
#define COLOR_BERRY         COLOR_RGB(0xE0, 0x4F, 0x90)
#define COLOR_OCEAN         COLOR_RGB(0x10, 0x80, 0xC0)
#define COLOR_LEAF          COLOR_RGB(0x60, 0xC0, 0x30)
#define COLOR_AMBER         COLOR_RGB(0xFF, 0xB0, 0x40)
#define COLOR_VIOLET        COLOR_RGB(0x90, 0x60, 0xFF)
#define COLOR_TEAL          COLOR_RGB(0x00, 0xB0, 0xA0)
#define COLOR_CORAL         COLOR_RGB(0xFF, 0x80, 0x70)
#define COLOR_SLATE         COLOR_RGB(0x60, 0x70, 0x80)

/* ------------------------------------------------------------------------- */
/* The colour word, said and read.
 *
 * "#rrggbb" is how this system spells a colour everywhere — in headers, in
 * commit messages, and now in what a person types at a program. These four
 * are the whole vocabulary: read one, write one, take one off the wheel,
 * and mix two. Integer arithmetic throughout; a cabin has no libm. */

/* "#rrggbb" or "rrggbb", either case, and nothing else — no names, no short
 * form, no trailing bytes. True on success, and only then is *out written. */
bool  color_parse(const char *text, Color *out);

/* The other direction. Exactly 7 characters and a NUL, so out[] is 8. */
void  color_format(Color c, char out[8]);

/* A colour off the wheel. `hue` runs 0..1535 — six sectors of 256, the same
 * scale the VGA text projection quantises on (boxos_color.h), so a hue that
 * reads as red on a framebuffer reads as red on a text console. `sat` and
 * `val` are 0..255; sat 0 is a grey of that value. */
Color color_wheel(uint16_t hue, uint8_t sat, uint8_t val);

/* ------------------------------------------------------------------------- */
/* Color state — process-local current text colors. Used by print/println and
 * the printf output path when no inline %color override applies. */

void  set_color(Color fg);
Color get_color(void);
void  set_color_bg(Color bg);
Color get_color_bg(void);

#ifdef __cplusplus
}
#endif

#endif /* BOX_COLOR_H */
