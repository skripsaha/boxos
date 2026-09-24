#ifndef KTYPES_H
#define KTYPES_H

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef __cplusplus
    #if !defined(bool) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202300L)
        typedef _Bool bool;
    #endif

    #ifndef true
        #define true  1
    #endif

    #ifndef false
        #define false 0
    #endif
#else
    #ifndef true
        #define true  true
    #endif
    #ifndef false
        #define false false
    #endif
#endif

#define __bool_true_false_are_defined 1

typedef unsigned long size_t;
typedef long          ptrdiff_t;
typedef long          ssize_t;

typedef unsigned long uintptr_t;
typedef long          intptr_t;

typedef signed char        int8_t;
typedef unsigned char      uint8_t;

typedef signed short       int16_t;
typedef unsigned short     uint16_t;

typedef signed int         int32_t;
typedef unsigned int       uint32_t;

typedef signed long long   int64_t;
typedef unsigned long long uint64_t;

typedef int8_t    int_least8_t;
typedef uint8_t   uint_least8_t;
typedef int16_t   int_least16_t;
typedef uint16_t  uint_least16_t;
typedef int32_t   int_least32_t;
typedef uint32_t  uint_least32_t;
typedef int64_t   int_least64_t;
typedef uint64_t  uint_least64_t;

typedef int8_t    int_fast8_t;
typedef uint8_t   uint_fast8_t;
typedef int64_t   int_fast16_t;
typedef uint64_t  uint_fast16_t;
typedef int64_t   int_fast32_t;
typedef uint64_t  uint_fast32_t;
typedef int64_t   int_fast64_t;
typedef uint64_t  uint_fast64_t;

typedef int64_t   intmax_t;
typedef uint64_t  uintmax_t;

#define INT8_MIN   (-128)
#define INT8_MAX   (127)
#define UINT8_MAX  (255U)

#define INT16_MIN  (-32768)
#define INT16_MAX  (32767)
#define UINT16_MAX (65535U)

#define INT32_MIN  (-2147483647 - 1)
#define INT32_MAX  (2147483647)
#define UINT32_MAX (4294967295U)

#define INT64_MIN  (-9223372036854775807LL - 1)
#define INT64_MAX  (9223372036854775807LL)
#define UINT64_MAX (18446744073709551615ULL)

#define INTPTR_MIN  INT64_MIN
#define INTPTR_MAX  INT64_MAX
#define UINTPTR_MAX UINT64_MAX

#define SIZE_MAX    UINT64_MAX
#define SSIZE_MAX   INT64_MAX
#define PTRDIFF_MIN INT64_MIN
#define PTRDIFF_MAX INT64_MAX

#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX

/*
 * ‼ THE COMPILER'S, NOT A TRICK THAT HAPPENS TO WORK.
 *
 * This was `((size_t)&(((type*)0)->member))`, which every C codebase has
 * carried at some point and which is undefined behaviour by the letter of the
 * standard: it forms a member designator on a null pointer and then takes its
 * address. It works on every compiler anybody has shipped, and it is still the
 * wrong tool when the compiler offers the right one — __builtin_offsetof is
 * what GCC's own <stddef.h> expands offsetof to, and it is a constant
 * expression by construction rather than by luck.
 *
 * It also removes a genuine split in this tree. `acpi_shutdown.c` included
 * <stddef.h> on top of this header, so THAT file was using the builtin while
 * every other file used the trick: two definitions of one word, and the one in
 * force depended on include order. Now there is one, and the outlier include is
 * gone with it.
 *
 * The guard matters: a freestanding compiler is required to provide <stddef.h>
 * (C17 §4p6), and if anything ever includes it before this header, its
 * definition is already in scope and identical.
 */
#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#define container_of(ptr, type, member) ({                      \
    const typeof(((type *)0)->member) *__mptr = (ptr);          \
    (type *)((char *)__mptr - offsetof(type, member));          \
})

#define __packed       __attribute__((packed))
#define __aligned(x)   __attribute__((aligned(x)))
#define __noreturn     __attribute__((noreturn))
#define __unused       __attribute__((unused))
#define __used         __attribute__((used))
#define __section(s)   __attribute__((section(s)))
#define __weak         __attribute__((weak))

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __BYTE_ORDER    __LITTLE_ENDIAN

typedef uint64_t physaddr_t;
typedef uint64_t virtaddr_t;

typedef volatile uint8_t  vu8;
typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;
typedef volatile uint64_t vu64;

#define IS_ALIGNED(addr, align) (((addr) & ((align) - 1)) == 0)

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define BIT(n)                (1ULL << (n))
#define BIT_SET(val, bit)     ((val) |= BIT(bit))
#define BIT_CLEAR(val, bit)   ((val) &= ~BIT(bit))
#define BIT_TOGGLE(val, bit)  ((val) ^= BIT(bit))
#define BIT_CHECK(val, bit)   (((val) >> (bit)) & 1)

#define STATIC_ASSERT(expr, msg) _Static_assert(expr, msg)

#endif // KTYPES_H
