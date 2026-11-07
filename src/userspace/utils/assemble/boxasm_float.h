#ifndef BOXASM_FLOAT_H
#define BOXASM_FLOAT_H

#include "box/defs.h"

/* The IEEE 754 bits of a decimal floating-point constant as written —
 * digits, a '.', digits, an optional exponent, '_' anywhere among the
 * digits — rounded to nearest, ties to even, straight from its digits into
 * binary32 (Width 4) or binary64 (Width 8). A value too large for the width,
 * or one not zero that would round to zero, is -1 with the reason in *Why. */
int BoxAsmFloatBits(const char *Text, size_t Length, int Negative, uint8_t Width,
                    uint64_t *Bits, const char **Why);

/* __Infinity__, __QNaN__ (or __NaN__) and __SNaN__ as NASM spells them:
 * 1 with their bits, 0 for any other word. */
int BoxAsmFloatSpecial(const char *Text, size_t Length, int Negative, uint8_t Width,
                       uint64_t *Bits);

#endif
