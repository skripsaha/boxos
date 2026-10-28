#ifndef BOX_CORE_FORMAT_H
#define BOX_CORE_FORMAT_H

#include <stdarg.h>

#include "box/defs.h"

/* Where formatted text goes. Put receives it in order, in pieces of any
 * size. Color, when the sink has one, takes BoxOS's %color (Background 0)
 * and %bgcolor (Background 1); a sink without it prints them as written. */
typedef struct BoxFormatSink BoxFormatSink;
struct BoxFormatSink {
    void (*Put)(BoxFormatSink *Sink, const char *Bytes, size_t Count);
    void (*Color)(BoxFormatSink *Sink, int Background, uint32_t Rgb);
};

/* The conversions of C's fprintf — flags, width, precision and their '*',
 * the length modifiers hh h l ll j z t L, and d i o u x X c s p n f F e E g G
 * a A % — with every floating-point value written exactly: its decimal
 * digits are the value's own, rounded to nearest, ties to even, at the
 * precision asked. No memory is allocated. Returns the bytes produced, or
 * -1 when there is no format, a wide character has no UTF-8 encoding, or the
 * count passes INT_MAX. */
int BoxFormat(BoxFormatSink *Sink, const char *Format, va_list Args);

#endif
