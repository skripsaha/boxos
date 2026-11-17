#ifndef PAINTER_CORE_SIMD_H
#define PAINTER_CORE_SIMD_H

/* simd.h — the processor's vector instructions, for lanes_sse2.cpp and
 * lanes_avx2.cpp. GCC's headers ask for the C library's stdlib.h only to give
 * _mm_malloc, which the system has no C library for and the engine never
 * uses: its header is marked as read already, and nothing else is missed. */

#if defined(__x86_64__)
#define _MM_MALLOC_H_INCLUDED
#include <immintrin.h>
#endif

#endif  // PAINTER_CORE_SIMD_H
