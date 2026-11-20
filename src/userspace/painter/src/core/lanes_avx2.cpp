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
#include "paint_rows.h"

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

/* The nearest number of each step of sixteen, widened to 32 bits for
 * looking up eight at once (BoxColorCode). */
struct Lookups {
    std::int32_t Nearest[4096];
};

const Lookups &Tables() noexcept
{
    static const Lookups Made = [] {
        Lookups T{};
        for (std::uint32_t I = 0; I < 4096u; I++) T.Nearest[I] = static_cast<std::int32_t>(BoxColorCodeOf[I]);
        return T;
    }();
    return Made;
}

/* Eight lights' numbers: the nearest, or dithered by Thresholds — each
 * light's entry of the dither table gathered as the low half of a word. */
AVX2 inline __m256i Codes(__m256i Light, const Lookups &T, const int *Table, const __m256i *Thresholds)
{
    if (!Thresholds) return _mm256_i32gather_epi32(T.Nearest, _mm256_srli_epi32(Light, 4), 4);
    const __m256i Entry = _mm256_and_si256(_mm256_i32gather_epi32(Table, Light, 2), _mm256_set1_epi32(0xFFFF));
    const __m256i N     = _mm256_and_si256(Entry, _mm256_set1_epi32(255));
    return _mm256_sub_epi32(N, _mm256_cmpgt_epi32(_mm256_srli_epi32(Entry, 8), *Thresholds));
}

AVX2 void Show(std::uint32_t *Out, const Pixel *Row, std::uint32_t Count, std::uint32_t Format,
               const std::uint8_t *Thresholds)
{
    const light::DitherTable *Dither = Thresholds ? light::DitherTableOfLight() : nullptr;
    if (Thresholds && !Dither) {
        Plain().Show(Out, Row, Count, Format, Thresholds);
        return;
    }
    const Lookups &T       = Tables();
    const int     *Table   = Dither ? reinterpret_cast<const int *>(Dither->Of) : nullptr;
    const __m256i  Halves  = _mm256_setr_epi32(0, 2, 4, 6, 1, 3, 5, 7);
    const __m256i  Sixteen = _mm256_set1_epi32(0xFFFF);
    const bool     Swap    = Format == BOX_PIXEL_RGB;
    std::uint32_t  I       = 0;
    for (; I + 8u <= Count; I += 8u) {
        /* Eight pixels as two words each, red and green, blue and alpha:
         * the reds-and-greens of all eight together, then the rest. */
        const __m256i Front = _mm256_permutevar8x32_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(Row + I)), Halves);
        const __m256i Back  = _mm256_permutevar8x32_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(Row + I + 4u)), Halves);
        const __m256i RG    = _mm256_permute2x128_si256(Front, Back, 0x20);
        const __m256i BA    = _mm256_permute2x128_si256(Front, Back, 0x31);
        __m256i       Thr   = _mm256_setzero_si256();
        if (Thresholds) Thr = _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(Thresholds + I)));
        const __m256i *Th = Thresholds ? &Thr : nullptr;
        const __m256i R   = Codes(_mm256_and_si256(RG, Sixteen), T, Table, Th);
        const __m256i G   = Codes(_mm256_srli_epi32(RG, 16), T, Table, Th);
        const __m256i B   = Codes(_mm256_and_si256(BA, Sixteen), T, Table, Th);
        const __m256i High = Swap ? B : R, Low = Swap ? R : B;
        const __m256i Packed =
            _mm256_or_si256(_mm256_or_si256(_mm256_slli_epi32(High, 16), _mm256_slli_epi32(G, 8)), Low);
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(Out + I), Packed);
    }
    if (I < Count) Plain().Show(Out + I, Row + I, Count - I, Format, Thresholds ? Thresholds + I : nullptr);
}

/* Four walks' sums over UNIT to the nearest, halves away from 0 — as
 * BoxShapeRound gives each. */
AVX2 inline __m256i Rounded(__m256i Sums)
{
    const __m256i Sign = _mm256_cmpgt_epi64(_mm256_setzero_si256(), Sums);
    const __m256i Size = _mm256_sub_epi64(_mm256_xor_si256(Sums, Sign), Sign);
    const __m256i Down = _mm256_srli_epi64(_mm256_add_epi64(Size, _mm256_set1_epi64x(BOX_SHAPE_UNIT / 2)), 16);
    return _mm256_sub_epi64(_mm256_xor_si256(Down, Sign), Sign);
}

AVX2 inline __m256i Size64(__m256i Of)
{
    const __m256i Sign = _mm256_cmpgt_epi64(_mm256_setzero_si256(), Of);
    return _mm256_sub_epi64(_mm256_xor_si256(Of, Sign), Sign);
}

/* Four sums, the first First and each Step past the one before. */
AVX2 inline __m256i Stepped(std::int64_t First, std::int64_t Step)
{
    return _mm256_setr_epi64x(First, First + Step, First + 2 * Step, First + 3 * Step);
}

/* For each of four pixels, the eight words a 128-bit Pair holds — two pixels
 * one after the other — weighed by the two weights Weights holds for it and
 * summed: the very sums of a mix of two, in 32 bits. */
AVX2 inline __m128i Weighed(__m128i Pair, __m256i Weights)
{
    const __m256i Made = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(Pair), Weights);
    return _mm_add_epi32(_mm256_castsi256_si128(Made), _mm256_extracti128_si256(Made, 1));
}

/* Pixel K's two weights out of eight — First and Second for each of four
 * pixels — each over its pixel's four channels. */
AVX2 inline __m256i WeightsOf(__m256i Pairs, int K)
{
    const __m256i Which = _mm256_setr_epi32(2 * K, 2 * K, 2 * K, 2 * K, 2 * K + 1, 2 * K + 1, 2 * K + 1, 2 * K + 1);
    return _mm256_permutevar8x32_epi32(Pairs, Which);
}

AVX2 inline __m256i Interleaved(__m128i First, __m128i Second)
{
    return _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_unpacklo_epi32(First, Second)), _mm_unpackhi_epi32(First, Second), 1);
}

/* A gradient's colour between the two kept at Table[Index] and the one after,
 * Weights its pixel's part of the way and the rest. */
AVX2 inline __m128i Between(const Pixel *Table, std::int64_t Index, __m256i Weights)
{
    const __m128i Two = _mm_loadu_si128(reinterpret_cast<const __m128i *>(Table + Index));
    return _mm_srli_epi32(_mm_add_epi32(Weighed(Two, Weights), _mm_set1_epi32(8)), 4);
}

/* A picture's colour mixed of the two pixels from Up and the two below them. */
AVX2 inline __m128i Mixed(const Pixel *Up, std::size_t Stride, __m256i Ups, __m256i Downs)
{
    const __m128i Sum = _mm_add_epi32(Weighed(_mm_loadu_si128(reinterpret_cast<const __m128i *>(Up)), Ups),
                                      Weighed(_mm_loadu_si128(reinterpret_cast<const __m128i *>(Up + Stride)), Downs));
    return _mm_srli_epi32(_mm_add_epi32(Sum, _mm_set1_epi32(32768)), 16);
}

/* Four pixels' channels, each under 65536, as four pixels. */
AVX2 inline __m256i Packed(__m128i P0, __m128i P1, __m128i P2, __m128i P3)
{
    return _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_packus_epi32(P0, P1)), _mm_packus_epi32(P2, P3), 1);
}

/* Four ways, each a whole number, brought within the gradient's ends as
 * Extend says; Clear set for each that is clear there. */
AVX2 inline __m256i Within(__m256i Way, std::uint32_t Extend, __m256i &Clear)
{
    const __m256i Zero = _mm256_setzero_si256(), Whole = _mm256_set1_epi64x(BOX_PAINT_GRADIENT);
    Clear              = Zero;
    if (Extend == BOX_PAINT_REPEAT) return _mm256_and_si256(Way, _mm256_set1_epi64x(BOX_PAINT_GRADIENT - 1));
    if (Extend == BOX_PAINT_REFLECT) {
        const __m256i Twice = _mm256_and_si256(Way, _mm256_set1_epi64x(2 * BOX_PAINT_GRADIENT - 1));
        return _mm256_blendv_epi8(Twice, _mm256_sub_epi64(_mm256_set1_epi64x(2 * BOX_PAINT_GRADIENT), Twice),
                                  _mm256_cmpgt_epi64(Twice, Whole));
    }
    const __m256i Before = _mm256_cmpgt_epi64(Zero, Way), Past = _mm256_cmpgt_epi64(Way, Whole);
    if (Extend == BOX_PAINT_NONE) {
        Clear = _mm256_or_si256(Before, Past);
        return _mm256_andnot_si256(Clear, Way);
    }
    return _mm256_blendv_epi8(_mm256_andnot_si256(Before, Way), Whole, Past);
}

/* The whole numbers nearest below the square roots of four sums of squares,
 * each under 2^52: a double's root of each, exact, lies within one of the
 * true one, and a step either way makes it exact — BoxShapeRoot64's very
 * number. */
AVX2 inline __m256i Roots(__m256i Squares)
{
    const __m256i Exponent = _mm256_set1_epi64x(0x4330000000000000ll);
    const __m256d Seen     = _mm256_sub_pd(_mm256_castsi256_pd(_mm256_or_si256(Squares, Exponent)), _mm256_castsi256_pd(Exponent));
    __m256i       Root     = _mm256_cvtepu32_epi64(_mm256_cvttpd_epi32(_mm256_sqrt_pd(Seen)));
    Root = _mm256_add_epi64(Root, _mm256_cmpgt_epi64(_mm256_mul_epu32(Root, Root), Squares));
    const __m256i Next = _mm256_add_epi64(Root, _mm256_set1_epi64x(1));
    return _mm256_sub_epi64(Root, _mm256_andnot_si256(_mm256_cmpgt_epi64(_mm256_mul_epu32(Next, Next), Squares), _mm256_set1_epi64x(-1)));
}

/* A linear gradient's or one drawn from its middle's colours, four pixels at
 * once: their ways worked in 64 bits, each pixel's two colours either side
 * read as one and mixed by its part of the way — the very numbers of
 * paint_rows.h. Any other gradient, and a radial one's pixels whose squares
 * reach past what a double holds whole, one at a time. */
AVX2 void Gradient(Pixel *Out, const GradientWalk &Walk, std::uint32_t Count)
{
    const BoxPaint *P      = Walk.Paint;
    const bool      Radial = P->Kind == BOX_PAINT_RADIAL;
    if (P->Kind != BOX_PAINT_LINEAR && !(Radial && P->FocalX == 0 && P->FocalY == 0)) {
        GradientPlain(Out, Walk, Walk.U, Walk.V, Count);
        return;
    }
    const __m256i Far = _mm256_set1_epi64x(std::int64_t{1} << 40), Reach = _mm256_set1_epi64x((std::int64_t{1} << 31) - 1);
    const __m256i Most = _mm256_set1_epi64x((std::int64_t{1} << 52) - 1);
    const __m256i StepU = _mm256_set1_epi64x(4 * Walk.StepU), StepV = _mm256_set1_epi64x(4 * Walk.StepV);
    __m256i       SumU = Stepped(Walk.U, Walk.StepU), SumV = Stepped(Walk.V, Walk.StepV);
    std::int64_t  U = Walk.U, V = Walk.V;
    std::uint32_t I = 0;
    for (; I + 4u <= Count; I += 4u, U += 4 * Walk.StepU, V += 4 * Walk.StepV) {
        const __m256i Across = Rounded(SumU);
        SumU                 = _mm256_add_epi64(SumU, StepU);
        __m256i Way          = Across;
        if (Radial) {
            const __m256i Down = Rounded(SumV);
            SumV               = _mm256_add_epi64(SumV, StepV);
            const __m256i SizeU = Size64(Across), SizeV = Size64(Down);
            const __m256i Away = _mm256_or_si256(_mm256_cmpgt_epi64(SizeU, Reach), _mm256_cmpgt_epi64(SizeV, Reach));
            const __m256i Squares = _mm256_add_epi64(_mm256_mul_epu32(SizeU, SizeU), _mm256_mul_epu32(SizeV, SizeV));
            if (_mm256_movemask_epi8(_mm256_andnot_si256(Away, _mm256_cmpgt_epi64(Squares, Most)))) {
                GradientPlain(Out + I, Walk, U, V, 4);
                continue;
            }
            Way = _mm256_blendv_epi8(Roots(_mm256_andnot_si256(Away, Squares)), Far, Away);
        }
        __m256i       Clear;
        const __m256i At    = Within(Way, P->Extend, Clear);
        alignas(32) std::int64_t Index[4];
        _mm256_store_si256(reinterpret_cast<__m256i *>(Index), _mm256_srli_epi64(At, 4));
        const __m128i Part  = _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(_mm256_and_si256(At, _mm256_set1_epi64x(15)),
                                                                                  _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6)));
        const __m256i Pairs = Interleaved(_mm_sub_epi32(_mm_set1_epi32(16), Part), Part);
        const __m256i Four  = Packed(Between(Walk.Table, Index[0], WeightsOf(Pairs, 0)), Between(Walk.Table, Index[1], WeightsOf(Pairs, 1)),
                                     Between(Walk.Table, Index[2], WeightsOf(Pairs, 2)), Between(Walk.Table, Index[3], WeightsOf(Pairs, 3)));
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(Out + I), _mm256_andnot_si256(Clear, Four));
    }
    GradientPlain(Out + I, Walk, U, V, Count - I);
}

/* A smooth picture's colours, four pixels at once where all four lie within
 * it with the four round each: their places worked in 64 bits, each pixel's
 * two rows of two read as they lie and mixed by nearness — the very numbers
 * of paint_rows.h; four of which any lies by the edge, one at a time. */
AVX2 void Picture(Pixel *Out, const PictureWalk &Walk, std::uint32_t Count)
{
    const BoxPaint *P = Walk.Paint;
    if (std::uint64_t{P->PictureStride} * P->PictureHeight >= (std::uint64_t{1} << 31)) {
        PicturePlain(Out, Walk, Walk.U, Walk.V, Count);
        return;
    }
    const __m256i Reach = _mm256_set1_epi64x((std::int64_t{1} << 30) - 1);
    const __m256i Lows  = _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6);
    const __m128i Half = _mm_set1_epi32(BOX_SHAPE_ONE / 2), Fine = _mm_set1_epi32(BOX_SHAPE_ONE - 1), One = _mm_set1_epi32(BOX_SHAPE_ONE);
    const __m128i Wide = _mm_set1_epi32(static_cast<int>(P->PictureWidth) - 1), High = _mm_set1_epi32(static_cast<int>(P->PictureHeight) - 1);
    const __m128i Stride = _mm_set1_epi32(static_cast<int>(P->PictureStride));
    const __m256i StepU = _mm256_set1_epi64x(4 * Walk.StepU), StepV = _mm256_set1_epi64x(4 * Walk.StepV);
    const __m256i Opacity = _mm256_set1_epi16(static_cast<short>(Walk.Opacity));
    const bool    Seen    = Walk.Opacity < light::Whole;
    __m256i       SumU = Stepped(Walk.U, Walk.StepU), SumV = Stepped(Walk.V, Walk.StepV);
    std::int64_t  U = Walk.U, V = Walk.V;
    std::uint32_t I = 0;
    for (; I + 4u <= Count; I += 4u, U += 4 * Walk.StepU, V += 4 * Walk.StepV) {
        const __m256i Across = Rounded(SumU), Down = Rounded(SumV);
        SumU = _mm256_add_epi64(SumU, StepU);
        SumV = _mm256_add_epi64(SumV, StepV);
        if (_mm256_movemask_epi8(_mm256_or_si256(_mm256_cmpgt_epi64(Size64(Across), Reach), _mm256_cmpgt_epi64(Size64(Down), Reach)))) {
            PicturePlain(Out + I, Walk, U, V, 4);
            continue;
        }
        /* Each place in 32 bits, less half a pixel: the pixel nearest below
         * the middles round it, and how far past that middle it lies. */
        const __m128i PU = _mm_sub_epi32(_mm256_castsi256_si128(_mm256_permutevar8x32_epi32(Across, Lows)), Half);
        const __m128i PV = _mm_sub_epi32(_mm256_castsi256_si128(_mm256_permutevar8x32_epi32(Down, Lows)), Half);
        const __m128i TI = _mm_srai_epi32(PU, 8), TJ = _mm_srai_epi32(PV, 8);
        const __m128i Out4 = _mm_or_si128(_mm_or_si128(_mm_cmplt_epi32(TI, _mm_setzero_si128()), _mm_cmplt_epi32(TJ, _mm_setzero_si128())),
                                          _mm_or_si128(_mm_cmpgt_epi32(_mm_add_epi32(TI, _mm_set1_epi32(1)), Wide),
                                                       _mm_cmpgt_epi32(_mm_add_epi32(TJ, _mm_set1_epi32(1)), High)));
        if (_mm_movemask_epi8(Out4)) {
            PicturePlain(Out + I, Walk, U, V, 4);
            continue;
        }
        const __m128i FX = _mm_and_si128(PU, Fine), FY = _mm_and_si128(PV, Fine);
        const __m128i RX = _mm_sub_epi32(One, FX), RY = _mm_sub_epi32(One, FY);
        const __m256i Ups   = Interleaved(_mm_mullo_epi32(RX, RY), _mm_mullo_epi32(FX, RY));
        const __m256i Downs = Interleaved(_mm_mullo_epi32(RX, FY), _mm_mullo_epi32(FX, FY));
        alignas(16) std::int32_t Index[4];
        _mm_store_si128(reinterpret_cast<__m128i *>(Index), _mm_add_epi32(_mm_mullo_epi32(TJ, Stride), TI));
        const std::size_t Below = P->PictureStride;
        const __m256i     Four  = Packed(Mixed(Walk.Texels + Index[0], Below, WeightsOf(Ups, 0), WeightsOf(Downs, 0)),
                                         Mixed(Walk.Texels + Index[1], Below, WeightsOf(Ups, 1), WeightsOf(Downs, 1)),
                                         Mixed(Walk.Texels + Index[2], Below, WeightsOf(Ups, 2), WeightsOf(Downs, 2)),
                                         Mixed(Walk.Texels + Index[3], Below, WeightsOf(Ups, 3), WeightsOf(Downs, 3)));
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(Out + I), Seen ? Times(Four, Opacity) : Four);
    }
    PicturePlain(Out + I, Walk, U, V, Count - I);
}

#undef AVX2

}  // namespace

const Kit &Avx2() noexcept
{
    static const Kit Made{LayColor, LayPaint, LayRow, Show, Gradient, Picture};
    return Made;
}

}  // namespace painter::core::lanes

#endif
