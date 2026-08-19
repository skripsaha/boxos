#ifndef BOX_DEFS_H
#define BOX_DEFS_H

typedef __UINT8_TYPE__   uint8_t;
typedef __UINT16_TYPE__  uint16_t;
typedef __UINT32_TYPE__  uint32_t;
typedef __UINT64_TYPE__  uint64_t;
typedef __INT8_TYPE__    int8_t;
typedef __INT16_TYPE__   int16_t;
typedef __INT32_TYPE__   int32_t;
typedef __INT64_TYPE__   int64_t;

typedef __SIZE_TYPE__      size_t;
typedef __PTRDIFF_TYPE__   ptrdiff_t;
typedef long               ssize_t;
typedef __UINTPTR_TYPE__   uintptr_t;
typedef __INTPTR_TYPE__    intptr_t;

// C23+ and C++ have bool as a keyword; older C standards need the typedef
#ifndef __cplusplus
#if !defined(__bool_true_false_are_defined) && (__STDC_VERSION__ < 202311L)
typedef _Bool bool;
#define true  1
#define false 0
#define __bool_true_false_are_defined 1
#endif
#endif

#ifdef __cplusplus
#define NULL nullptr
#else
#define NULL ((void*)0)
#endif

/* #ifndef-guarded for the same reason the limits below are: a C++ TU may see
 * <cstdarg> first — which is the compiler's own <stdarg.h> — and it spells
 * these with the same builtins. The definitions are identical, so the only
 * thing an unguarded redefinition produced was four warnings, but "identical
 * today" is not a property worth relying on. Found by <cstdio>, which is the
 * first header to include both this file and <cstdarg>. */
typedef __builtin_va_list va_list;
#ifndef va_start
#define va_start(ap, last) __builtin_va_start(ap, last)
#endif
#ifndef va_end
#define va_end(ap)         __builtin_va_end(ap)
#endif
#ifndef va_arg
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#endif
#ifndef va_copy
#define va_copy(d, s)      __builtin_va_copy(d, s)
#endif

/* #ifndef-guarded: C++ TUs may see boxcxx's <cstdint> first, which
 * defines the same limits via compiler builtins (equal values, different
 * spelling). Either include order must stay warning-free. */
#ifndef UINT8_MAX
#define UINT8_MAX   0xFF
#endif
#ifndef UINT16_MAX
#define UINT16_MAX  0xFFFF
#endif
#ifndef UINT32_MAX
#define UINT32_MAX  0xFFFFFFFFU
#endif
#ifndef UINT64_MAX
#define UINT64_MAX  0xFFFFFFFFFFFFFFFFULL
#endif
#ifndef INT8_MAX
#define INT8_MAX    127
#endif
#ifndef INT16_MAX
#define INT16_MAX   32767
#endif
#ifndef INT32_MAX
#define INT32_MAX   2147483647
#endif
#ifndef INT64_MAX
#define INT64_MAX   9223372036854775807LL
#endif
#ifndef INT8_MIN
#define INT8_MIN    (-128)
#endif
#ifndef INT16_MIN
#define INT16_MIN   (-32768)
#endif
#ifndef INT32_MIN
#define INT32_MIN   (-2147483647 - 1)
#endif
#ifndef INT64_MIN
#define INT64_MIN   (-9223372036854775807LL - 1)
#endif
#ifndef SIZE_MAX
#define SIZE_MAX    UINT64_MAX
#endif

#endif // BOX_DEFS_H
