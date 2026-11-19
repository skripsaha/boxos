/*
 * lanes_avx2.cpp — the engine's rows worked four pixels at a time with AVX2
 * (Haswell and later), and written to the glass eight at a time, the numbers
 * looked up eight at once: the very numbers of light.h, bit for bit. Only
 * these functions use AVX2, each marked so; nothing else of the engine is
 * built for it, so a processor without it never meets an AVX2 instruction.
 * See lanes.h.
 */

#include "lanes.h"

#if defined(__x86_64__)

#include "simd.h"

#include "boxos_pixel.h"
#include "light.h"

namespace painter::core::lanes {

namespace {

#define AVX2 __attribute__((target("avx2")))

/* Sixteen shares of 65535 times sixteen: light::Times in each — each 128-bit
 * half worked as SSE2 works its eight. */
AVX2 inline __m256i Times(__m256i A, __m256i B)
{
    const __m256i Low = _mm256_mullo_epi16(A, B), High = _mm256_mulhi_epu16(A, B);
    const __m256i Half = _mm256_set1_epi32(32768);
    __m256i       P0   = _mm256_add_epi32(_mm256_unpacklo_epi16(Low, High), Half);
    __m256i       P1   = _mm256_add_epi32(_mm256_unpackhi_epi16(Low, High), Half);
    P0                 = _mm256_srli_epi32(_mm256_add_epi32(P0, _mm256_srli_epi32(P0, 16)), 16);
    P1                 = _mm256_srli_epi32(_mm256_add_epi32(P1, _mm256_srli_epi32(P1, 16)), 16);
    const __m256i Packed = _mm256_packs_epi32(_mm256_sub_epi32(P0, Half), _mm256_sub_epi32(P1, Half));
    return _mm256_xor_si256(Packed, _mm256_set1_epi16(static_cast<short>(0x8000)));
}

/* Four pixels: Above at Share laid over Below — light::Over. */
AVX2 inline __m256i Over(__m256i Below, __m256i Above, __m256i Share)
{
    const __m256i Laid  = Times(Above, Share);
    const __m256i Alpha = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(Laid, 0xFF), 0xFF);
    const __m256i Left  = _mm256_xor_si256(Alpha, _mm256_set1_epi16(-1));
    return _mm256_add_epi16(Laid, Times(Below, Left));
}

/* The shares of four pixels, each in all four of its channels. */
AVX2 inline __m256i FourShares(const std::uint16_t *Share)
{
    const __m128i Four  = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(Share));
    const __m128i Pairs = _mm_unpacklo_epi16(Four, Four);
    return _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_unpacklo_epi32(Pairs, Pairs)),
                                   _mm_unpackhi_epi32(Pairs, Pairs), 1);
}

AVX2 void LayColor(Pixel *Row, const std::uint16_t *Share, std::uint32_t Count, Pixel Color)
{
    std::uint64_t Bits;
    __builtin_memcpy(&Bits, &Color, sizeof Bits);
    const __m256i Above = _mm256_set1_epi64x(static_cast<long long>(Bits));
    std::uint32_t I     = 0;
    for (; I + 4u <= Count; I += 4u) {
        std::uint64_t None;
        __builtin_memcpy(&None, Share + I, sizeof None);
        if (!None) continue;   /* four pixels not covered: left as they are */
        __m256i *At = reinterpret_cast<__m256i *>(Row + I);
        _mm256_storeu_si256(At, Over(_mm256_loadu_si256(At), Above, FourShares(Share + I)));
    }
    for (; I < Count; I++)
        if (Share[I]) Row[I] = light::Over(Row[I], Color, Share[I]);
}

AVX2 void LayPaint(Pixel *Row, const Pixel *Paint, const std::uint16_t *Share, std::uint32_t Count)
{
    std::uint32_t I = 0;
    for (; I + 4u <= Count; I += 4u) {
        std::uint64_t None;
        __builtin_memcpy(&None, Share + I, sizeof None);
        if (!None) continue;
        __m256i *At = reinterpret_cast<__m256i *>(Row + I);
        _mm256_storeu_si256(At, Over(_mm256_loadu_si256(At),
                                     _mm256_loadu_si256(reinterpret_cast<const __m256i *>(Paint + I)), FourShares(Share + I)));
    }
    for (; I < Count; I++)
        if (Share[I]) Row[I] = light::Over(Row[I], Paint[I], Share[I]);
}

AVX2 void LayRow(Pixel *Row, const Pixel *From, std::uint32_t Count, std::uint32_t Share)
{
    const __m256i Shares = _mm256_set1_epi16(static_cast<short>(Share));
    const __m256i Whole  = _mm256_set1_epi16(-1);
    const bool    Copy   = Share == light::Whole;
    std::uint32_t I      = 0;
    for (; I + 4u <= Count; I += 4u) {
        __m256i      *At    = reinterpret_cast<__m256i *>(Row + I);
        const __m256i Paint = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(From + I));
        /* Four whole pixels laid whole: the paint itself (lanes_plain.cpp). */
        if (Copy && (static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi16(Paint, Whole))) & 0xC0C0C0C0u) == 0xC0C0C0C0u) {
            _mm256_storeu_si256(At, Paint);
            continue;
        }
        _mm256_storeu_si256(At, Over(_mm256_loadu_si256(At), Paint, Shares));
    }
    for (; I < Count; I++) Row[I] = Copy && From[I].Alpha == light::Whole ? From[I] : light::Over(Row[I], From[I], Share);
}

/* The tables the glass's numbers are looked up in, widened to 32 bits for
 * looking up eight at once: the nearest number of each step of sixteen, the
 * number at or below it, each number's light — and past the last, a light no
 * pixel reaches — and the reciprocal of each gap (light.h). */
struct Lookups {
    std::int32_t Nearest[4096];
    std::int32_t Floor[4096];
    std::int32_t Light[257];
    std::int32_t Reciprocal[256];
};

const Lookups &Tables() noexcept
{
    static const Lookups Made = [] {
        Lookups             T{};
        const light::Steps &S = light::StepsOfLight();
        for (std::uint32_t I = 0; I < 4096u; I++) {
            T.Nearest[I] = static_cast<std::int32_t>(BoxColorCodeOf[I]);
            T.Floor[I]   = S.Floor[I];
        }
        for (std::uint32_t N = 0; N < 256u; N++) {
            T.Light[N]      = static_cast<std::int32_t>(BoxColorLight(N));
            T.Reciprocal[N] = static_cast<std::int32_t>(S.Reciprocal[N]);
        }
        T.Light[256] = 0x7FFFFFFF;
        return T;
    }();
    return Made;
}

/* Eight lights' numbers: the nearest, or dithered by Thresholds. */
AVX2 inline __m256i Codes(__m256i Light, const Lookups &T, const __m256i *Thresholds)
{
    const __m256i Step = _mm256_srli_epi32(Light, 4);
    if (!Thresholds) return _mm256_i32gather_epi32(T.Nearest, Step, 4);
    __m256i       N    = _mm256_i32gather_epi32(T.Floor, Step, 4);
    const __m256i One  = _mm256_set1_epi32(1);
    const __m256i Next = _mm256_i32gather_epi32(T.Light, _mm256_add_epi32(N, One), 4);
    N = _mm256_sub_epi32(N, _mm256_cmpgt_epi32(_mm256_add_epi32(Light, One), Next));
    const __m256i Low  = _mm256_i32gather_epi32(T.Light, N, 4);
    const __m256i Gap  = _mm256_i32gather_epi32(T.Reciprocal, N, 4);
    const __m256i Part = _mm256_srli_epi32(_mm256_mullo_epi32(_mm256_sub_epi32(Light, Low), Gap), 16);
    return _mm256_sub_epi32(N, _mm256_cmpgt_epi32(Part, *Thresholds));
}

AVX2 void Show(std::uint32_t *Out, const Pixel *Row, std::uint32_t Count, std::uint32_t Format,
               const std::uint8_t *Thresholds)
{
    const Lookups &T       = Tables();
    const __m256i  Pairs   = _mm256_setr_epi32(0, 2, 4, 6, 8, 10, 12, 14);
    const __m256i  Sixteen = _mm256_set1_epi32(0xFFFF);
    const bool     Swap    = Format == BOX_PIXEL_RGB;
    std::uint32_t  I       = 0;
    for (; I + 8u <= Count; I += 8u) {
        const int    *Base = reinterpret_cast<const int *>(Row + I);
        const __m256i RG   = _mm256_i32gather_epi32(Base, Pairs, 4);
        const __m256i BA   = _mm256_i32gather_epi32(Base + 1, Pairs, 4);
        __m256i       Thr = _mm256_setzero_si256();
        if (Thresholds) Thr = _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(Thresholds + I)));
        const __m256i *Th = Thresholds ? &Thr : nullptr;
        const __m256i R   = Codes(_mm256_and_si256(RG, Sixteen), T, Th);
        const __m256i G   = Codes(_mm256_srli_epi32(RG, 16), T, Th);
        const __m256i B   = Codes(_mm256_and_si256(BA, Sixteen), T, Th);
        const __m256i High = Swap ? B : R, Low = Swap ? R : B;
        const __m256i Packed =
            _mm256_or_si256(_mm256_or_si256(_mm256_slli_epi32(High, 16), _mm256_slli_epi32(G, 8)), Low);
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(Out + I), Packed);
    }
    if (I < Count) Plain().Show(Out + I, Row + I, Count - I, Format, Thresholds ? Thresholds + I : nullptr);
}

#undef AVX2

}  // namespace

const Kit &Avx2() noexcept
{
    static const Kit Made{LayColor, LayPaint, LayRow, Show};
    return Made;
}

}  // namespace painter::core::lanes

#endif
