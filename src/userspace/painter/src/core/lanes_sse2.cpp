/*
 * lanes_sse2.cpp — the engine's rows worked two pixels at a time with SSE2,
 * which every x86-64 has: the very numbers of light.h, bit for bit. Writing
 * to the glass looks numbers up one by one, as the plain way does, SSE2
 * having no way to look up eight at once. See lanes.h.
 */

#include "lanes.h"

#if defined(__x86_64__)

#include "simd.h"

#include "light.h"

namespace painter::core::lanes {

namespace {

/* Eight shares of 65535 times eight: light::Times in each, the products'
 * rounding carried once more past the shift, the sums written back into
 * sixteen bits through a signed pack with the halfway point taken away. */
__attribute__((target("sse2"))) inline __m128i Times(__m128i A, __m128i B)
{
    const __m128i Low = _mm_mullo_epi16(A, B), High = _mm_mulhi_epu16(A, B);
    const __m128i Half = _mm_set1_epi32(32768);
    __m128i       P0   = _mm_add_epi32(_mm_unpacklo_epi16(Low, High), Half);
    __m128i       P1   = _mm_add_epi32(_mm_unpackhi_epi16(Low, High), Half);
    P0                 = _mm_srli_epi32(_mm_add_epi32(P0, _mm_srli_epi32(P0, 16)), 16);
    P1                 = _mm_srli_epi32(_mm_add_epi32(P1, _mm_srli_epi32(P1, 16)), 16);
    const __m128i Packed = _mm_packs_epi32(_mm_sub_epi32(P0, Half), _mm_sub_epi32(P1, Half));
    return _mm_xor_si128(Packed, _mm_set1_epi16(static_cast<short>(0x8000)));
}

/* Two pixels: Above at Share laid over Below — light::Over. */
__attribute__((target("sse2"))) inline __m128i Over(__m128i Below, __m128i Above, __m128i Share)
{
    const __m128i Laid  = Times(Above, Share);
    const __m128i Alpha = _mm_shufflehi_epi16(_mm_shufflelo_epi16(Laid, 0xFF), 0xFF);
    const __m128i Left  = _mm_xor_si128(Alpha, _mm_set1_epi16(-1));
    return _mm_add_epi16(Laid, Times(Below, Left));
}

/* The shares of two pixels, each in all four of its channels. */
__attribute__((target("sse2"))) inline __m128i TwoShares(const std::uint16_t *Share)
{
    const __m128i Two = _mm_cvtsi32_si128(static_cast<int>(Share[0] | static_cast<std::uint32_t>(Share[1]) << 16));
    const __m128i Pairs = _mm_unpacklo_epi16(Two, Two);
    return _mm_unpacklo_epi32(Pairs, Pairs);
}

__attribute__((target("sse2"))) void LayColor(Pixel *Row, const std::uint16_t *Share, std::uint32_t Count, Pixel Color)
{
    std::uint64_t Bits;
    __builtin_memcpy(&Bits, &Color, sizeof Bits);
    const __m128i Above = _mm_set1_epi64x(static_cast<long long>(Bits));
    std::uint32_t I     = 0;
    for (; I + 2u <= Count; I += 2u) {
        __m128i *At = reinterpret_cast<__m128i *>(Row + I);
        _mm_storeu_si128(At, Over(_mm_loadu_si128(At), Above, TwoShares(Share + I)));
    }
    for (; I < Count; I++)
        if (Share[I]) Row[I] = light::Over(Row[I], Color, Share[I]);
}

__attribute__((target("sse2"))) void LayPaint(Pixel *Row, const Pixel *Paint, const std::uint16_t *Share,
                                              std::uint32_t Count)
{
    std::uint32_t I = 0;
    for (; I + 2u <= Count; I += 2u) {
        __m128i *At = reinterpret_cast<__m128i *>(Row + I);
        _mm_storeu_si128(At, Over(_mm_loadu_si128(At), _mm_loadu_si128(reinterpret_cast<const __m128i *>(Paint + I)),
                                  TwoShares(Share + I)));
    }
    for (; I < Count; I++)
        if (Share[I]) Row[I] = light::Over(Row[I], Paint[I], Share[I]);
}

__attribute__((target("sse2"))) void LayRow(Pixel *Row, const Pixel *From, std::uint32_t Count, std::uint32_t Share)
{
    const __m128i Shares = _mm_set1_epi16(static_cast<short>(Share));
    std::uint32_t I      = 0;
    for (; I + 2u <= Count; I += 2u) {
        __m128i *At = reinterpret_cast<__m128i *>(Row + I);
        _mm_storeu_si128(At, Over(_mm_loadu_si128(At), _mm_loadu_si128(reinterpret_cast<const __m128i *>(From + I)), Shares));
    }
    for (; I < Count; I++) Row[I] = light::Over(Row[I], From[I], Share);
}

}  // namespace

const Kit &Sse2() noexcept
{
    static const Kit Made{LayColor, LayPaint, LayRow, Plain().Show};
    return Made;
}

}  // namespace painter::core::lanes

#endif
