// boxcxx — <charconv> floating-point to_chars (Ф9A-2a: no-precision overloads)
//
// Own Ryu (Adams, PLDI 2018) shortest round-trip for float and double, plus
// the explicit scientific/fixed/general/hex no-precision formats. The Ryu
// powers-of-five tables are COMPUTED at first use via exact big-integer
// arithmetic — no magic constants to mistype, and the generation is provably
// correct (5^i is exact). Validated char-for-char against a reference
// std::to_chars over ~30M random + edge values before being committed here.
//
// The precision overloads (5-arg) and to_string(float/double) arrive in the
// next installment (Ф9A-2b) on top of the same big-integer machinery; this
// file deliberately ships only the shortest/no-precision surface.
//
// Not constexpr: the standard does not require constexpr float conversion,
// and the table state is runtime-initialised.
#include <charconv>
#include <cstdint>

// from_chars(long double)'s exact-midpoint path (see kLdHeapWords) allocates a
// bounded big-integer scratch on the boxlib heap. malloc/free are strand-safe
// (the boxlib heap serialises every allocation) and malloc returns null (never
// throws) on exhaustion, so from_chars degrades to the stack path, never throws.
extern "C" {
void *_malloc_impl(__SIZE_TYPE__ size);
void  free(void *ptr);
}

namespace {

using u32  = uint32_t;
using u64  = uint64_t;
using i32  = int32_t;
using u128 = unsigned __int128;

// ── minimal big integer (little-endian base 2^32) ────────────────────────
// 720 words (~23040 bits) is sized for the long double (80-bit x87) surface,
// whose exact-decimal machinery dwarfs float/double. The binding case is
// from_chars of a subnormal at the parser's 1290-digit cap: it builds
// den = 10^~6241 (~648 words) and shifts num by up to ~16507 bits (~651 words),
// so the original Dragon4-only sizing of 576 silently saturated and misrounded
// those inputs; 720 holds the ~651-word worst case with margin. Dragon4
// (to_chars) scales M by up to 2^16446 (≈514 words). The float/double paths
// only ever touch ≤168 words (their magnitude pre-clamp bounds the operands),
// so the larger array is byte-for-byte inert for them. Every grow primitive
// ALSO saturates at kBigWords (see BigMulSmall / BigShl / BigInc / BigDivMod128),
// so an out-of-range exponent that slips past the magnitude pre-clamp in
// ParseFp / ParseFpLd can still never write past the array.
constexpr int kBigWords = 720;
struct BigInt {
    u32 w[kBigWords];
    int n;  // number of significant words
};

void BigSetU64(BigInt &a, u64 v)
{
    a.w[0] = (u32)v;
    a.w[1] = (u32)(v >> 32);
    a.n    = a.w[1] ? 2 : (a.w[0] ? 1 : 0);
}
void BigSetU128(BigInt &a, u128 v)
{
    for (int i = 0; i < 4; ++i) { a.w[i] = (u32)v; v >>= 32; }
    a.n = 4; while (a.n > 0 && a.w[a.n - 1] == 0) --a.n;
}
void BigMulSmall(BigInt &a, u32 m)
{
    u64 carry = 0;
    for (int i = 0; i < a.n; ++i) {
        u64 p   = (u64)a.w[i] * m + carry;
        a.w[i]  = (u32)p;
        carry   = p >> 32;
    }
    while (carry && a.n < kBigWords) { a.w[a.n++] = (u32)carry; carry >>= 32; }
}
int BigBitLen(const BigInt &a)
{
    if (a.n == 0) return 0;
    int bits = (a.n - 1) * 32;
    u32 hi   = a.w[a.n - 1];
    while (hi) { ++bits; hi >>= 1; }
    return bits;
}
void BigShr(const BigInt &a, int s, BigInt &out)
{
    int wsh = s / 32, bsh = s % 32;
    out.n = 0;
    for (int i = 0; i + wsh < a.n; ++i) {
        u64 lo = a.w[i + wsh];
        u64 hi = (i + wsh + 1 < a.n) ? a.w[i + wsh + 1] : 0;
        u32 v  = bsh ? (u32)((lo >> bsh) | (hi << (32 - bsh))) : (u32)lo;
        out.w[i] = v;
        if (v) out.n = i + 1;
    }
}
void BigShl(const BigInt &a, int s, BigInt &out)
{
    int wsh = s / 32, bsh = s % 32;
    for (int i = 0; i < kBigWords; ++i) out.w[i] = 0;
    for (int i = 0; i < a.n; ++i) {
        u64 v  = (u64)a.w[i] << bsh;
        int lo = i + wsh, hi = lo + 1;
        if (lo >= 0 && lo < kBigWords) out.w[lo] |= (u32)v;          // saturate: a shift
        if (hi >= 0 && hi < kBigWords) out.w[hi] |= (u32)(v >> 32);  // can never write OOB
    }
    out.n = a.n + wsh + 1;
    if (out.n > kBigWords) out.n = kBigWords;
    while (out.n > 0 && out.w[out.n - 1] == 0) --out.n;
}
void BigLow128(const BigInt &a, u64 &lo, u64 &hi)
{
    u64 w0 = a.n > 0 ? a.w[0] : 0, w1 = a.n > 1 ? a.w[1] : 0;
    u64 w2 = a.n > 2 ? a.w[2] : 0, w3 = a.n > 3 ? a.w[3] : 0;
    lo = w0 | (w1 << 32);
    hi = w2 | (w3 << 32);
}
// Low 128 bits of ceil(N / D) (N, D > 0); bit-by-bit schoolbook division.
void BigDivCeilLow128(const BigInt &N, const BigInt &D, u64 &qlo, u64 &qhi)
{
    BigInt rem; rem.n = 0;
    BigInt q; for (int i = 0; i < kBigWords; ++i) q.w[i] = 0; q.n = 0;
    int nb = BigBitLen(N);
    for (int bit = nb - 1; bit >= 0; --bit) {
        BigInt tmp; BigShl(rem, 1, tmp); rem = tmp;
        if ((N.w[bit / 32] >> (bit % 32)) & 1) { if (rem.n == 0) rem.n = 1; rem.w[0] |= 1; }
        int cmp = 0;
        int m   = rem.n > D.n ? rem.n : D.n;
        for (int i = m - 1; i >= 0; --i) {
            u32 rv = i < rem.n ? rem.w[i] : 0, dv = i < D.n ? D.w[i] : 0;
            if (rv != dv) { cmp = rv > dv ? 1 : -1; break; }
        }
        if (cmp >= 0) {
            u64 borrow = 0;
            for (int i = 0; i < rem.n; ++i) {
                u64 dv  = i < D.n ? D.w[i] : 0;
                u64 cur = (u64)rem.w[i] - dv - borrow;
                rem.w[i] = (u32)cur;
                borrow   = (cur >> 63) & 1 ? 1 : 0;
            }
            while (rem.n > 0 && rem.w[rem.n - 1] == 0) --rem.n;
            q.w[bit / 32] |= (1u << (bit % 32));
            if (bit / 32 + 1 > q.n) q.n = bit / 32 + 1;
        }
    }
    BigLow128(q, qlo, qhi);
    if (rem.n > 0) { if (++qlo == 0) ++qhi; }  // ceil
}
u32 BigDivModSmall(BigInt &a, u32 d)
{
    u64 rem = 0;
    for (int i = a.n - 1; i >= 0; --i) {
        u64 cur = (rem << 32) | a.w[i];
        a.w[i]  = (u32)(cur / d);
        rem     = cur % d;
    }
    while (a.n > 0 && a.w[a.n - 1] == 0) --a.n;
    return (u32)rem;
}
void BigInc(BigInt &a)
{
    int i = 0;
    for (;;) {
        if (i >= a.n) { if (a.n < kBigWords) a.w[a.n++] = 1; break; }
        if (a.w[i] != 0xFFFFFFFFu) { ++a.w[i]; break; }
        a.w[i] = 0; ++i;
    }
}
int BigToDec(BigInt a, char *out)
{
    if (a.n == 0) { out[0] = '0'; return 1; }
    char tmp[1200]; int n = 0;
    while (a.n > 0) tmp[n++] = (char)('0' + BigDivModSmall(a, 10));
    for (int i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    return n;
}

// ── Ryu tables (computed once) ────────────────────────────────────────────
constexpr int kDoublePow5InvBitcount = 125;
constexpr int kDoublePow5Bitcount    = 125;
constexpr int kFloatPow5InvBitcount  = 59;
constexpr int kFloatPow5Bitcount     = 61;

u64  g_dpow5[326][2];
u64  g_dpow5inv[342][2];
u64  g_fpow5[47];
u64  g_fpow5inv[31];
bool g_tablesReady = false;

inline u32 Pow5Bits(i32 e) { return (u32)(((e * 1217359) >> 19) + 1); }
inline i32 Log10Pow2(i32 e) { return (i32)((((u32)e) * 78913) >> 18); }
inline i32 Log10Pow5(i32 e) { return (i32)((((u32)e) * 732923) >> 20); }

void InitTables()
{
    BigInt p; BigSetU64(p, 1);
    for (int i = 0; i < 326; ++i) {
        int    b = BigBitLen(p);
        BigInt v;
        int    shift = b - kDoublePow5Bitcount;
        if (shift > 0) BigShr(p, shift, v); else BigShl(p, -shift, v);
        BigLow128(v, g_dpow5[i][0], g_dpow5[i][1]);
        BigMulSmall(p, 5);
    }
    BigSetU64(p, 1);
    for (int q = 0; q < 342; ++q) {
        int    e = Pow5Bits(q) - 1 + kDoublePow5InvBitcount;
        BigInt num, one; BigSetU64(one, 1); BigShl(one, e, num);
        BigDivCeilLow128(num, p, g_dpow5inv[q][0], g_dpow5inv[q][1]);
        BigMulSmall(p, 5);
    }
    BigSetU64(p, 1);
    for (int i = 0; i < 47; ++i) {
        int    b = BigBitLen(p);
        BigInt v;
        int    shift = b - kFloatPow5Bitcount;
        if (shift > 0) BigShr(p, shift, v); else BigShl(p, -shift, v);
        u64 lo, hi; BigLow128(v, lo, hi); g_fpow5[i] = lo;
        BigMulSmall(p, 5);
    }
    BigSetU64(p, 1);
    for (int q = 0; q < 31; ++q) {
        int    e = Pow5Bits(q) - 1 + kFloatPow5InvBitcount;
        BigInt num, one; BigSetU64(one, 1); BigShl(one, e, num);
        u64 lo, hi; BigDivCeilLow128(num, p, lo, hi); g_fpow5inv[q] = lo;
        BigMulSmall(p, 5);
    }
    g_tablesReady = true;
}
inline void EnsureTables() { if (!g_tablesReady) InitTables(); }  // single cabin: no race

// ── Ryu shortest core ─────────────────────────────────────────────────────
inline u64 MulShift64(u64 m, const u64 *mul, i32 j)
{
    u128 b0 = (u128)m * mul[0];
    u128 b2 = (u128)m * mul[1];
    return (u64)(((b0 >> 64) + b2) >> (j - 64));
}
inline u64 MulShiftAll64(u64 m, const u64 *mul, i32 j, u64 *vp, u64 *vm, u32 mmShift)
{
    *vp = MulShift64(4 * m + 2, mul, j);
    *vm = MulShift64(4 * m - 1 - mmShift, mul, j);
    return MulShift64(4 * m, mul, j);
}
inline u32 MulShift32(u32 m, u64 factor, i32 shift) { return (u32)(((u128)m * factor) >> shift); }
inline u32 Pow5Factor64(u64 v) { u32 c = 0; for (;;) { u64 q = v / 5; if (v - 5 * q) break; v = q; ++c; } return c; }
inline bool MultipleOfPow5_64(u64 v, u32 p) { return Pow5Factor64(v) >= p; }
inline bool MultipleOfPow2_64(u64 v, u32 p) { return (v & ((1ull << p) - 1)) == 0; }
inline bool MultipleOfPow5_32(u32 v, u32 p) { u32 c = 0; for (;;) { u32 q = v / 5; if (v - 5 * q) break; v = q; ++c; } return c >= p; }
inline bool MultipleOfPow2_32(u32 v, u32 p) { return (v & ((1u << p) - 1)) == 0; }
inline u32 DecimalLength17(u64 v) { u32 n = 0; do { ++n; v /= 10; } while (v); return n; }

struct FloatDec { u64 mantissa; i32 exponent; };

FloatDec D2D(u64 ieeeMantissa, u32 ieeeExponent)
{
    i32 e2; u64 m2;
    if (ieeeExponent == 0) { e2 = 1 - 1023 - 52 - 2; m2 = ieeeMantissa; }
    else { e2 = (i32)ieeeExponent - 1023 - 52 - 2; m2 = (1ull << 52) | ieeeMantissa; }
    bool even = (m2 & 1) == 0, acceptBounds = even;
    u64 mv      = 4 * m2;
    u32 mmShift = (ieeeMantissa != 0) || (ieeeExponent <= 1);
    u64 vr, vp, vm; i32 e10;
    bool vmTZ = false, vrTZ = false;
    if (e2 >= 0) {
        u32 q = Log10Pow2(e2) - (e2 > 3);
        e10   = (i32)q;
        i32 k = kDoublePow5InvBitcount + (i32)Pow5Bits((i32)q) - 1;
        i32 i = -e2 + (i32)q + k;
        vr    = MulShiftAll64(m2, g_dpow5inv[q], i, &vp, &vm, mmShift);
        if (q <= 21) {
            if (mv % 5 == 0) vrTZ = MultipleOfPow5_64(mv, q);
            else if (acceptBounds) vmTZ = MultipleOfPow5_64(mv - 1 - mmShift, q);
            else vp -= MultipleOfPow5_64(mv + 2, q);
        }
    } else {
        u32 q = Log10Pow5(-e2) - (-e2 > 1);
        e10   = (i32)q + e2;
        i32 i = -e2 - (i32)q;
        i32 k = (i32)Pow5Bits(i) - kDoublePow5Bitcount;
        i32 j = (i32)q - k;
        vr    = MulShiftAll64(m2, g_dpow5[i], j, &vp, &vm, mmShift);
        if (q <= 1) { vrTZ = true; if (acceptBounds) vmTZ = mmShift == 1; else --vp; }
        else if (q < 63) vrTZ = MultipleOfPow2_64(mv, q);
    }
    i32 removed = 0; uint8_t lastRemoved = 0; u64 output;
    if (vmTZ || vrTZ) {
        for (;;) {
            u64 vpd = vp / 10, vmd = vm / 10;
            if (vpd <= vmd) break;
            u32 vmr = (u32)(vm - 10 * vmd);
            u64 vrd = vr / 10; u32 vrr = (u32)(vr - 10 * vrd);
            vmTZ &= vmr == 0; vrTZ &= lastRemoved == 0;
            lastRemoved = (uint8_t)vrr; vr = vrd; vp = vpd; vm = vmd; ++removed;
        }
        if (vmTZ) for (;;) {
            u64 vmd = vm / 10; u32 vmr = (u32)(vm - 10 * vmd);
            if (vmr != 0) break;
            u64 vpd = vp / 10, vrd = vr / 10; u32 vrr = (u32)(vr - 10 * vrd);
            vrTZ &= lastRemoved == 0; lastRemoved = (uint8_t)vrr;
            vr = vrd; vp = vpd; vm = vmd; ++removed;
        }
        if (vrTZ && lastRemoved == 5 && vr % 2 == 0) lastRemoved = 4;
        output = vr + ((vr == vm && (!acceptBounds || !vmTZ)) || lastRemoved >= 5);
    } else {
        bool roundUp = false;
        u64 vpd100 = vp / 100, vmd100 = vm / 100;
        if (vpd100 > vmd100) {
            u64 vrd100 = vr / 100; u32 vrr100 = (u32)(vr - 100 * vrd100);
            roundUp = vrr100 >= 50; vr = vrd100; vp = vpd100; vm = vmd100; removed += 2;
        }
        for (;;) {
            u64 vpd = vp / 10, vmd = vm / 10;
            if (vpd <= vmd) break;
            u64 vrd = vr / 10; u32 vrr = (u32)(vr - 10 * vrd);
            roundUp = vrr >= 5; vr = vrd; vp = vpd; vm = vmd; ++removed;
        }
        output = vr + (vr == vm || roundUp);
    }
    return {output, e10 + removed};
}

FloatDec F2D(u32 ieeeMantissa, u32 ieeeExponent)
{
    i32 e2; u32 m2;
    if (ieeeExponent == 0) { e2 = 1 - 127 - 23 - 2; m2 = ieeeMantissa; }
    else { e2 = (i32)ieeeExponent - 127 - 23 - 2; m2 = (1u << 23) | ieeeMantissa; }
    bool even = (m2 & 1) == 0, acceptBounds = even;
    u32 mv = 4 * m2, mp = 4 * m2 + 2;
    u32 mmShift = (ieeeMantissa != 0) || (ieeeExponent <= 1);
    u32 mm = 4 * m2 - 1 - mmShift;
    u32 vr, vp, vm; i32 e10;
    bool vmTZ = false, vrTZ = false; uint8_t lastRemoved = 0;
    if (e2 >= 0) {
        u32 q = Log10Pow2(e2); e10 = (i32)q;
        i32 k = kFloatPow5InvBitcount + (i32)Pow5Bits((i32)q) - 1;
        i32 i = -e2 + (i32)q + k;
        vr = MulShift32(mv, g_fpow5inv[q], i);
        vp = MulShift32(mp, g_fpow5inv[q], i);
        vm = MulShift32(mm, g_fpow5inv[q], i);
        if (q != 0 && (vp - 1) / 10 <= vm / 10) {
            i32 l = kFloatPow5InvBitcount + (i32)Pow5Bits((i32)q - 1) - 1;
            lastRemoved = (uint8_t)(MulShift32(mv, g_fpow5inv[q - 1], -e2 + (i32)q - 1 + l) % 10);
        }
        if (q <= 9) {
            if (mv % 5 == 0) vrTZ = MultipleOfPow5_32(mv, q);
            else if (acceptBounds) vmTZ = MultipleOfPow5_32(mm, q);
            else vp -= MultipleOfPow5_32(mp, q);
        }
    } else {
        u32 q = Log10Pow5(-e2); e10 = (i32)q + e2;
        i32 i = -e2 - (i32)q;
        i32 k = (i32)Pow5Bits(i) - kFloatPow5Bitcount;
        i32 j = (i32)q - k;
        vr = MulShift32(mv, g_fpow5[i], j);
        vp = MulShift32(mp, g_fpow5[i], j);
        vm = MulShift32(mm, g_fpow5[i], j);
        if (q != 0 && (vp - 1) / 10 <= vm / 10) {
            j = (i32)q - 1 - ((i32)Pow5Bits(i + 1) - kFloatPow5Bitcount);
            lastRemoved = (uint8_t)(MulShift32(mv, g_fpow5[i + 1], j) % 10);
        }
        if (q <= 1) { vrTZ = true; if (acceptBounds) vmTZ = mmShift == 1; else --vp; }
        else if (q < 31) vrTZ = MultipleOfPow2_32(mv, q - 1);
    }
    i32 removed = 0; u32 output;
    if (vmTZ || vrTZ) {
        while (vp / 10 > vm / 10) {
            vmTZ &= (vm % 10) == 0; vrTZ &= lastRemoved == 0;
            lastRemoved = (uint8_t)(vr % 10); vr /= 10; vp /= 10; vm /= 10; ++removed;
        }
        if (vmTZ) while (vm % 10 == 0) {
            vrTZ &= lastRemoved == 0; lastRemoved = (uint8_t)(vr % 10);
            vr /= 10; vp /= 10; vm /= 10; ++removed;
        }
        if (vrTZ && lastRemoved == 5 && vr % 2 == 0) lastRemoved = 4;
        output = vr + ((vr == vm && (!acceptBounds || !vmTZ)) || lastRemoved >= 5);
    } else {
        while (vp / 10 > vm / 10) { lastRemoved = (uint8_t)(vr % 10); vr /= 10; vp /= 10; vm /= 10; ++removed; }
        output = vr + (vr == vm || lastRemoved >= 5);
    }
    return {output, e10 + removed};
}

// ── formatting ─────────────────────────────────────────────────────────────
enum { M_SHORTEST = 0, M_SCI = 1, M_FIXED = 2, M_GENERAL = 3 };

char *WriteExp(char *p, i32 e)
{
    *p++ = 'e';
    if (e < 0) { *p++ = '-'; e = -e; } else *p++ = '+';
    if (e >= 100) { *p++ = (char)('0' + e / 100); e %= 100; }
    *p++ = (char)('0' + e / 10);
    *p++ = (char)('0' + e % 10);
    return p;
}
int DecLen64(u64 v) { int n = 0; do { ++n; v /= 10; } while (v); return n; }

// round(fullMant * 2^e2) to nearest integer (ties→even), decimal digits.
int EmitExactInt(char *out, u64 fullMant, i32 e2)
{
    if (e2 >= 0) {
        BigInt b; BigSetU64(b, fullMant);
        BigInt s; BigShl(b, e2, s);
        return BigToDec(s, out);
    }
    int sh   = -e2;
    u64 q    = sh >= 64 ? 0 : (fullMant >> sh);
    u64 rem  = sh >= 64 ? fullMant : (fullMant & ((1ull << sh) - 1));
    u64 half = (sh >= 1 && sh <= 64) ? (1ull << (sh - 1)) : 0;
    if (rem > half || (rem == half && (q & 1))) ++q;
    char tmp[24]; int n = 0; do { tmp[n++] = (char)('0' + q % 10); q /= 10; } while (q);
    for (int i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    return n;
}

int EmitFp(char *out, bool sign, u64 mant, i32 exp, u64 fullMant, i32 e2, int mode)
{
    char *p = out;
    if (sign) *p++ = '-';
    char digits[20]; u32 olen = (u32)DecLen64(mant);
    for (i32 k = (i32)olen - 1; k >= 0; --k) { digits[k] = (char)('0' + mant % 10); mant /= 10; }
    i32 e_sci = exp + (i32)olen - 1;

    i32  sci_len = (i32)olen + (olen > 1 ? 1 : 0) + 2 + ((e_sci >= 100 || e_sci <= -100) ? 3 : 2);
    char fixedbuf[400]; int fixed_len;
    if (exp > 0) {
        fixed_len = EmitExactInt(fixedbuf, fullMant, e2);
    } else if (exp == 0) {
        for (u32 k = 0; k < olen; ++k) fixedbuf[k] = digits[k];
        fixed_len = (int)olen;
    } else {
        i32 dot = (i32)olen + exp; int n = 0;
        if (dot <= 0) {
            fixedbuf[n++] = '0'; fixedbuf[n++] = '.';
            for (i32 z = 0; z < -dot; ++z) fixedbuf[n++] = '0';
            for (u32 k = 0; k < olen; ++k) fixedbuf[n++] = digits[k];
        } else {
            for (i32 k = 0; k < dot; ++k) fixedbuf[n++] = digits[k];
            fixedbuf[n++] = '.';
            for (u32 k = (u32)dot; k < olen; ++k) fixedbuf[n++] = digits[k];
        }
        fixed_len = n;
    }

    bool use_fixed;
    switch (mode) {
        case M_SCI:     use_fixed = false; break;
        case M_FIXED:   use_fixed = true; break;
        case M_GENERAL: use_fixed = (e_sci >= -4 && e_sci <= 5); break;
        default:        use_fixed = fixed_len <= sci_len; break;
    }
    if (use_fixed) {
        for (int k = 0; k < fixed_len; ++k) *p++ = fixedbuf[k];
    } else {
        *p++ = digits[0];
        if (olen > 1) { *p++ = '.'; for (u32 k = 1; k < olen; ++k) *p++ = digits[k]; }
        p = WriteExp(p, e_sci);
    }
    return (int)(p - out);
}

// Hexfloat (no "0x", lowercase, trailing zero nibbles stripped, normalized
// leading 1 for subnormals). fracBits holds fracHexDigits nibbles MSB-first.
int EmitHex(char *out, bool sign, u64 fracBits, int fracHexDigits, i32 binExp, int lead)
{
    char *p = out;
    if (sign) *p++ = '-';
    *p++ = (char)('0' + lead);
    int last = fracHexDigits - 1;
    while (last >= 0 && ((fracBits >> (4 * (fracHexDigits - 1 - last))) & 0xF) == 0) --last;
    if (last >= 0) {
        *p++ = '.';
        for (int k = 0; k <= last; ++k) {
            u32 nib = (u32)((fracBits >> (4 * (fracHexDigits - 1 - k))) & 0xF);
            *p++ = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
        }
    }
    *p++ = 'p';
    if (binExp < 0) { *p++ = '-'; binExp = -binExp; } else *p++ = '+';
    char tb[8]; int n = 0; do { tb[n++] = (char)('0' + binExp % 10); binExp /= 10; } while (binExp);
    for (int k = n - 1; k >= 0; --k) *p++ = tb[k];
    return (int)(p - out);
}

// Render into a scratch buffer; returns length. Handles inf/nan/zero.
int RenderDouble(char *out, double d, int mode, bool hex)
{
    u64  bits = __builtin_bit_cast(u64, d);
    bool sign = (bits >> 63) & 1;
    u64  mant = bits & ((1ull << 52) - 1);
    u32  exp  = (u32)((bits >> 52) & 0x7FF);
    if (exp == 0x7FF) {
        char *p = out; if (sign) *p++ = '-';
        const char *s = mant ? "nan" : "inf";
        *p++ = s[0]; *p++ = s[1]; *p++ = s[2];
        return (int)(p - out);
    }
    if (hex) {
        if (exp == 0 && mant == 0) return EmitHex(out, sign, 0, 13, 0, 0);
        i32 binExp; u64 frac;
        if (exp == 0) {
            int h = 51; while (h >= 0 && !((mant >> h) & 1)) --h;
            binExp = h - 1074; frac = (mant & (((u64)1 << h) - 1)) << (52 - h);
        } else { binExp = (i32)exp - 1023; frac = mant; }
        return EmitHex(out, sign, frac, 13, binExp, 1);
    }
    if (exp == 0 && mant == 0) return EmitFp(out, sign, 0, 0, 0, 0, mode);
    u64 fullMant = exp == 0 ? mant : ((1ull << 52) | mant);
    i32 e2       = (i32)(exp == 0 ? 1 : exp) - 1075;
    FloatDec v   = D2D(mant, exp);
    return EmitFp(out, sign, v.mantissa, v.exponent, fullMant, e2, mode);
}
int RenderFloat(char *out, float f, int mode, bool hex)
{
    u32  bits = __builtin_bit_cast(u32, f);
    bool sign = (bits >> 31) & 1;
    u32  mant = bits & ((1u << 23) - 1);
    u32  exp  = (bits >> 23) & 0xFF;
    if (exp == 0xFF) {
        char *p = out; if (sign) *p++ = '-';
        const char *s = mant ? "nan" : "inf";
        *p++ = s[0]; *p++ = s[1]; *p++ = s[2];
        return (int)(p - out);
    }
    if (hex) {
        if (exp == 0 && mant == 0) return EmitHex(out, sign, 0, 6, 0, 0);
        i32 binExp; u64 frac;
        if (exp == 0) {
            int h = 22; while (h >= 0 && !((mant >> h) & 1)) --h;
            binExp = h - 149; frac = ((u64)(mant & ((1u << h) - 1)) << (23 - h)) << 1;
        } else { binExp = (i32)exp - 127; frac = (u64)mant << 1; }
        return EmitHex(out, sign, frac, 6, binExp, 1);
    }
    if (exp == 0 && mant == 0) return EmitFp(out, sign, 0, 0, 0, 0, mode);
    u64 fullMant = exp == 0 ? mant : ((1u << 23) | mant);
    i32 e2       = (i32)(exp == 0 ? 1 : exp) - 150;
    FloatDec v   = F2D(mant, exp);
    return EmitFp(out, sign, v.mantissa, v.exponent, fullMant, e2, mode);
}

std::to_chars_result Commit(char *first, char *last, const char *scratch, int len)
{
    if (len > (last - first)) return {last, std::errc::value_too_large};
    for (int i = 0; i < len; ++i) first[i] = scratch[i];
    return {first + len, std::errc{}};
}

int FmtToMode(std::chars_format fmt, bool &hex)
{
    hex = fmt == std::chars_format::hex;
    if (fmt == std::chars_format::scientific) return M_SCI;
    if (fmt == std::chars_format::fixed) return M_FIXED;
    return M_GENERAL;  // general or hex (hex handled via the flag)
}

// ── precision (5-arg) path ─────────────────────────────────────────────────
// |value| = bigN / 10^fracCount, an exact finite decimal (a binary fraction
// is a finite decimal). round(value * 10^k) is then a plain big integer.
void BuildBigN(u64 fullMant, i32 e2, BigInt &bigN, int &fracCount)
{
    if (e2 >= 0) { BigSetU64(bigN, fullMant); BigInt o; BigShl(bigN, e2, o); bigN = o; fracCount = 0; }
    else { BigSetU64(bigN, fullMant); for (int i = 0; i < -e2; ++i) BigMulSmall(bigN, 5); fracCount = -e2; }
}
BigInt RoundScaled(BigInt bigN, int scale)  // round(bigN * 10^scale), ties→even
{
    if (scale >= 0) { for (int i = 0; i < scale; ++i) BigMulSmall(bigN, 10); return bigN; }
    int k = -scale, rd = 0; bool rest = false;
    for (int i = 0; i < k; ++i) { int d = (int)BigDivModSmall(bigN, 10); if (i == k - 1) rd = d; else if (d) rest = true; }
    if (rd > 5 || (rd == 5 && (rest || (bigN.n > 0 && (bigN.w[0] & 1))))) BigInc(bigN);
    return bigN;
}
int BigDecLen(BigInt a) { char t[1200]; return BigToDec(a, t); }

// Bounded output cursor — every precision formatter writes through it straight
// into the caller's [first, last). Trailing-zero runs are clamped to the
// buffer, so an arbitrarily large precision can never overrun: it simply
// reports overflow (→ value_too_large), the only correct limit being the
// caller's own buffer. The big-integer work is bounded by the value's exact
// decimal (≤ ~767 digits) — precision excess is emitted as zeros, never as a
// scaled-up big integer.
struct OutBuf {
    char       *p;
    char *const end;
    bool        of = false;
    void put(char c) { if (p < end) *p++ = c; else of = true; }
    void zeros(long n) { while (n-- > 0) put('0'); }
    void run(const char *s, int n) { for (int i = 0; i < n; ++i) put(s[i]); }
};

constexpr int kExactDigits = 800;  // bound on a value's exact significant digits

void FmtFixedP(OutBuf &o, bool sign, u64 fullMant, i32 e2, int p)
{
    BigInt bigN; int frac; BuildBigN(fullMant, e2, bigN, frac);
    if (sign) o.put('-');
    char S[kExactDigits];
    if (p < frac) {  // fewer fractional digits than exact → round (divide, bounded)
        int M = BigToDec(RoundScaled(bigN, p - frac), S);
        if (p == 0) { o.run(S, M); return; }
        if (M <= p) { o.put('0'); o.put('.'); o.zeros(p - M); o.run(S, M); }
        else { o.run(S, M - p); o.put('.'); o.run(S + (M - p), p); }
        return;
    }
    int M = BigToDec(bigN, S);  // exact digits + (p - frac) trailing zeros
    int intLen = M - frac;
    if (intLen <= 0) o.put('0'); else o.run(S, intLen);
    if (p > 0) {
        o.put('.');
        if (intLen <= 0) { o.zeros(-intLen); o.run(S, M); }
        else o.run(S + intLen, frac);
        o.zeros((long)p - frac);
    }
}
void FmtSciP(OutBuf &o, bool sign, u64 fullMant, i32 e2, int p)
{
    BigInt bigN; int frac; BuildBigN(fullMant, e2, bigN, frac);
    int L = BigDecLen(bigN); i32 decExp = L - frac - 1;
    if (sign) o.put('-');
    char S[kExactDigits]; i32 X;
    if (p + 1 < L) {  // round to p+1 significant digits (divide, bounded)
        int M = BigToDec(RoundScaled(bigN, p - L + 1), S);
        X = decExp + (M - (p + 1));
        o.put(S[0]);
        if (p > 0) { o.put('.'); o.run(S + 1, p); }
    } else {  // all L significant digits + (p+1-L) trailing zeros
        BigToDec(bigN, S); X = decExp;
        o.put(S[0]);
        if (p > 0) { o.put('.'); o.run(S + 1, L - 1); o.zeros((long)p - (L - 1)); }
    }
    char e[16]; o.run(e, (int)(WriteExp(e, X) - e));
}
void FmtGeneralP(OutBuf &o, bool sign, u64 fullMant, i32 e2, int p)
{
    int P = p < 1 ? 1 : p;
    BigInt bigN; int frac; BuildBigN(fullMant, e2, bigN, frac);
    int L = BigDecLen(bigN); i32 decExp = L - frac - 1;
    if (sign) o.put('-');
    char S[kExactDigits]; int M, X, sig;
    if (P < L) { M = BigToDec(RoundScaled(bigN, P - L), S); X = decExp + (M - P); sig = P; }
    else { M = BigToDec(bigN, S); X = decExp; sig = M; }  // P>=L: trailing zeros would strip
    if (X < -4 || X >= P) {
        int fracEnd = sig; while (fracEnd > 1 && S[fracEnd - 1] == '0') --fracEnd;
        o.put(S[0]);
        if (fracEnd > 1) { o.put('.'); o.run(S + 1, fracEnd - 1); }
        char e[16]; o.run(e, (int)(WriteExp(e, X) - e));
    } else if (X >= 0) {
        int intLen = X + 1;
        if (intLen >= sig) { o.run(S, sig); o.zeros((long)intLen - sig); }
        else { int fracEnd = sig; while (fracEnd > intLen && S[fracEnd - 1] == '0') --fracEnd;
               o.run(S, intLen);
               if (fracEnd > intLen) { o.put('.'); o.run(S + intLen, fracEnd - intLen); } }
    } else {
        int fracEnd = sig; while (fracEnd > 0 && S[fracEnd - 1] == '0') --fracEnd;
        o.put('0');
        if (fracEnd > 0) { o.put('.'); o.zeros(-X - 1); o.run(S, fracEnd); }
    }
}
void FmtZeroP(OutBuf &o, bool sign, int mode, int p)
{
    if (sign) o.put('-');
    if (mode == M_GENERAL) { o.put('0'); return; }
    o.put('0');
    if (p > 0) { o.put('.'); o.zeros(p); }
    if (mode == M_SCI) o.run("e+00", 4);
}
// Hex with precision does NOT normalize (subnormal keeps a leading 0; a
// rounding carry just grows the leading digit 0→1 or 1→2) — unlike the
// no-precision hex above.
void FmtHexP(OutBuf &o, bool sign, int lead, i32 binExp, u64 frac, int fracBits, int p)
{
    if (sign) o.put('-');
    int nibbles = fracBits / 4;  // 13 (double) or 6 (float, frac pre-shifted)
    if (p < nibbles) {
        int keepBits = 4 * p, shift = fracBits - keepBits;
        u64 kept  = keepBits ? (frac >> shift) : 0;
        u64 rmask = frac & ((shift >= 64) ? ~0ull : ((1ull << shift) - 1));
        u64 half  = (shift >= 1) ? (1ull << (shift - 1)) : 0;
        bool up = rmask > half || (rmask == half && (keepBits ? (kept & 1) : (lead & 1)));
        // keepBits==64 (only reachable at LD hex precision=16) makes 1ull<<keepBits
        // undefined, so detect the carry via wraparound instead.
        if (up) { if (keepBits >= 64) { if (++kept == 0) ++lead; }
                  else if (keepBits) { if (++kept == (1ull << keepBits)) { kept = 0; ++lead; } }
                  else ++lead; }
        o.put((char)('0' + lead));
        if (p > 0) { o.put('.'); for (int k = 0; k < p; ++k) { u32 nib = (u32)((kept >> (4 * (p - 1 - k))) & 0xF); o.put((char)(nib < 10 ? '0' + nib : 'a' + nib - 10)); } }
    } else {
        o.put((char)('0' + lead)); o.put('.');
        for (int k = 0; k < nibbles; ++k) { u32 nib = (u32)((frac >> (4 * (nibbles - 1 - k))) & 0xF); o.put((char)(nib < 10 ? '0' + nib : 'a' + nib - 10)); }
        o.zeros((long)p - nibbles);
    }
    o.put('p');
    if (binExp < 0) { o.put('-'); binExp = -binExp; } else o.put('+');
    char tb[8]; int n = 0; do { tb[n++] = (char)('0' + binExp % 10); binExp /= 10; } while (binExp);
    for (int k = n - 1; k >= 0; --k) o.put(tb[k]);
}
void RenderPrecDouble(OutBuf &o, double d, int mode, bool hex, int prec)
{
    u64 bits = __builtin_bit_cast(u64, d); bool sign = (bits >> 63) & 1;
    u64 mant = bits & ((1ull << 52) - 1); u32 exp = (u32)((bits >> 52) & 0x7FF);
    if (exp == 0x7FF) { if (sign) o.put('-'); o.run(mant ? "nan" : "inf", 3); return; }
    if (hex) {
        int lead; i32 binExp; u64 frac;
        if (exp == 0 && mant == 0) { lead = 0; binExp = 0; frac = 0; }
        else { lead = exp ? 1 : 0; binExp = exp ? (i32)exp - 1023 : -1022; frac = mant; }
        FmtHexP(o, sign, lead, binExp, frac, 52, prec); return;
    }
    if (exp == 0 && mant == 0) { FmtZeroP(o, sign, mode, prec); return; }
    u64 fullMant = exp == 0 ? mant : ((1ull << 52) | mant); i32 e2 = (i32)(exp == 0 ? 1 : exp) - 1075;
    if (mode == M_FIXED) FmtFixedP(o, sign, fullMant, e2, prec);
    else if (mode == M_SCI) FmtSciP(o, sign, fullMant, e2, prec);
    else FmtGeneralP(o, sign, fullMant, e2, prec);
}
void RenderPrecFloat(OutBuf &o, float f, int mode, bool hex, int prec)
{
    u32 bits = __builtin_bit_cast(u32, f); bool sign = (bits >> 31) & 1;
    u32 mant = bits & ((1u << 23) - 1); u32 exp = (bits >> 23) & 0xFF;
    if (exp == 0xFF) { if (sign) o.put('-'); o.run(mant ? "nan" : "inf", 3); return; }
    if (hex) {
        int lead; i32 binExp; u64 frac;
        if (exp == 0 && mant == 0) { lead = 0; binExp = 0; frac = 0; }
        else { lead = exp ? 1 : 0; binExp = exp ? (i32)exp - 127 : -126; frac = (u64)mant << 1; }
        FmtHexP(o, sign, lead, binExp, frac, 24, prec); return;
    }
    if (exp == 0 && mant == 0) { FmtZeroP(o, sign, mode, prec); return; }
    u64 fullMant = exp == 0 ? mant : ((1u << 23) | mant); i32 e2 = (i32)(exp == 0 ? 1 : exp) - 150;
    if (mode == M_FIXED) FmtFixedP(o, sign, fullMant, e2, prec);
    else if (mode == M_SCI) FmtSciP(o, sign, fullMant, e2, prec);
    else FmtGeneralP(o, sign, fullMant, e2, prec);
}

// ── from_chars (decimal/hex → float/double, correctly rounded) ─────────────
// Clinger fast-path for small double cases; otherwise exact big-integer
// rounding of value = num/den to the nearest representable value (ties→even),
// covering normal/subnormal/overflow/underflow.
int BigCmpFull(const BigInt &a, const BigInt &b)
{
    if (a.n != b.n) return a.n > b.n ? 1 : -1;
    for (int i = a.n - 1; i >= 0; --i)
        if (a.w[i] != b.w[i]) return a.w[i] > b.w[i] ? 1 : -1;
    return 0;
}
void BigSubFull(BigInt &a, const BigInt &b)  // a -= b, requires a >= b
{
    u64 borrow = 0;
    for (int i = 0; i < a.n; ++i) {
        u64 d = i < b.n ? b.w[i] : 0, c = (u64)a.w[i] - d - borrow;
        a.w[i] = (u32)c; borrow = (c >> 63) & 1;
    }
    while (a.n > 0 && a.w[a.n - 1] == 0) --a.n;
}
void BigAddFull(const BigInt &a, const BigInt &b, BigInt &out)  // out = a + b
{
    int m = a.n > b.n ? a.n : b.n; u64 carry = 0;
    for (int i = 0; i < m; ++i) {
        u64 s = (u64)(i < a.n ? a.w[i] : 0) + (i < b.n ? b.w[i] : 0) + carry;
        out.w[i] = (u32)s; carry = s >> 32;
    }
    out.n = m;
    if (carry && out.n < kBigWords) out.w[out.n++] = (u32)carry;
    while (out.n > 0 && out.w[out.n - 1] == 0) --out.n;
}
u64 BigDivModFull(const BigInt &num, const BigInt &den, BigInt &rem)  // q (low64) + rem
{
    rem.n = 0;
    BigInt q; for (int i = 0; i < kBigWords; ++i) q.w[i] = 0; q.n = 0;
    for (int bit = BigBitLen(num) - 1; bit >= 0; --bit) {
        BigInt t; BigShl(rem, 1, t); rem = t;
        if ((num.w[bit / 32] >> (bit % 32)) & 1) { if (rem.n == 0) rem.n = 1; rem.w[0] |= 1; }
        if (BigCmpFull(rem, den) >= 0) { BigSubFull(rem, den); q.w[bit / 32] |= (1u << (bit % 32)); if (bit / 32 + 1 > q.n) q.n = bit / 32 + 1; }
    }
    return (q.n > 0 ? q.w[0] : 0) | (q.n > 1 ? ((u64)q.w[1] << 32) : 0);
}
u64 RoundedQ(const BigInt &num, const BigInt &den, int shift)  // round(num*2^shift/den), ties→even
{
    BigInt sn, sd;
    if (shift >= 0) { BigShl(num, shift, sn); sd = den; }
    else { sn = num; BigShl(den, -shift, sd); }
    BigInt rem; u64 q = BigDivModFull(sn, sd, rem);
    BigInt rem2; BigShl(rem, 1, rem2); int c = BigCmpFull(rem2, sd);
    if (c > 0 || (c == 0 && (q & 1))) ++q;
    return q;
}
// 128-bit quotient of num/den + remainder — like BigDivModFull but keeps the
// low 128 quotient bits (long double's transient q reaches ~2^65, so a u64
// quotient truncates). BigDivModFull is left untouched for the float/double path.
void BigDivMod128(const BigInt &num, const BigInt &den, u128 &q, BigInt &rem)
{
    rem.n = 0; q = 0;
    for (int bit = BigBitLen(num) - 1; bit >= 0; --bit) {
        BigInt t; BigShl(rem, 1, t); rem = t;
        if ((num.w[bit / 32] >> (bit % 32)) & 1) { if (rem.n == 0) rem.n = 1; rem.w[0] |= 1; }
        if (BigCmpFull(rem, den) >= 0) { BigSubFull(rem, den); if (bit < 128) q |= ((u128)1 << bit); }
    }
}
u128 RoundedQ128(const BigInt &num, const BigInt &den, int shift)  // round(num*2^shift/den), ties→even
{
    BigInt sn, sd;
    if (shift >= 0) { BigShl(num, shift, sn); sd = den; }
    else { sn = num; BigShl(den, -shift, sd); }
    u128 q; BigInt rem; BigDivMod128(sn, sd, q, rem);
    BigInt rem2; BigShl(rem, 1, rem2); int c = BigCmpFull(rem2, sd);
    if (c > 0 || (c == 0 && (u64)(q & 1))) ++q;
    return q;
}
const double kPow10[23] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                           1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
u64 AssembleBits(bool neg, u64 ef, u64 mant, bool isFloat)
{
    return isFloat ? (((u64)neg << 31) | (ef << 23) | (mant & ((1u << 23) - 1)))
                   : (((u64)neg << 63) | (ef << 52) | (mant & ((1ull << 52) - 1)));
}
void RoundToIeee(const BigInt &num, const BigInt &den, int P, int emin, int emax, u64 &ef, u64 &mant, bool &oor)
{
    int e2 = BigBitLen(num) - BigBitLen(den) - 1;
    u64 q = RoundedQ(num, den, (P - 1) - e2); int g = 0;
    while (q >= (1ull << P)) { ++e2; q = RoundedQ(num, den, (P - 1) - e2); if (++g > 6) break; }
    while (q < (1ull << (P - 1))) { --e2; q = RoundedQ(num, den, (P - 1) - e2); if (++g > 6) break; }
    if (e2 > emax) { oor = true; ef = (u64)(emax - emin + 2); mant = 0; return; }
    if (e2 >= emin) { ef = (u64)(e2 - emin + 1); mant = q - (1ull << (P - 1)); return; }
    int subShift = -emin + (P - 1);
    u64 qs = RoundedQ(num, den, subShift);
    if (qs == 0) { oor = true; ef = 0; mant = 0; return; }
    if (qs >= (1ull << (P - 1))) { ef = 1; mant = qs - (1ull << (P - 1)); return; }
    ef = 0; mant = qs;
}
u64 DigitsToBits(bool neg, const char *dig, int ndig, int E, bool isFloat, bool &oor)
{
    oor = false;
    int i0 = 0; while (i0 < ndig && dig[i0] == '0') ++i0;
    int hi = ndig; while (hi > i0 && dig[hi - 1] == '0') { --hi; ++E; }
    if (hi - i0 == 0) return isFloat ? ((u64)neg << 31) : ((u64)neg << 63);
    if (!isFloat && hi - i0 <= 15 && E >= -22 && E <= 22) {
        u64 m = 0; for (int i = i0; i < hi; ++i) m = m * 10 + (dig[i] - '0');
        double d = (double)m; d = E >= 0 ? d * kPow10[E] : d / kPow10[-E]; if (neg) d = -d;
        return __builtin_bit_cast(u64, d);
    }
    BigInt num; BigSetU64(num, 0);
    for (int i = i0; i < hi; ++i) {
        BigMulSmall(num, 10);
        u64 carry = (u64)(dig[i] - '0');
        for (int k = 0; k < num.n && carry; ++k) { u64 s = (u64)num.w[k] + carry; num.w[k] = (u32)s; carry = s >> 32; }
        if (carry && num.n < kBigWords) num.w[num.n++] = (u32)carry;
    }
    BigInt den; BigSetU64(den, 1);
    if (E >= 0) { for (int i = 0; i < E; ++i) BigMulSmall(num, 10); }
    else { for (int i = 0; i < -E; ++i) BigMulSmall(den, 10); }
    u64 ef, mant; int P = isFloat ? 24 : 53, emin = isFloat ? -126 : -1022, emax = isFloat ? 127 : 1023;
    RoundToIeee(num, den, P, emin, emax, ef, mant, oor);
    return AssembleBits(neg, ef, mant, isFloat);
}
int HexVal(char c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; }
bool CharIs(char c, char l) { return (c | 0x20) == l; }

// Parse a float/double; returns consumed length, fills bits, sets ec (0/EINVAL=22/ERANGE=34).
int ParseFp(const char *first, const char *last, int mode, bool hex, bool isFloat, u64 &bits, int &ec)
{
    int P = isFloat ? 24 : 53; u64 infExp = isFloat ? 255 : 2047, quiet = 1ull << (P - 2);
    constexpr long kExpAccumCap = 1L << 40;  // bound exponent accumulation: long-safe, far past any representable exponent
    const char *p = first; bool neg = false;
    if (p < last && *p == '-') { neg = true; ++p; }
    if (p < last && CharIs(*p, 'i') && last - p >= 3 && CharIs(p[0], 'i') && CharIs(p[1], 'n') && CharIs(p[2], 'f')) {
        const char *q = p + 3;
        if (last - p >= 8 && CharIs(p[3], 'i') && CharIs(p[4], 'n') && CharIs(p[5], 'i') && CharIs(p[6], 't') && CharIs(p[7], 'y')) q = p + 8;
        bits = AssembleBits(neg, infExp, 0, isFloat); ec = 0; return (int)(q - first);
    }
    if (p < last && CharIs(*p, 'n') && last - p >= 3 && CharIs(p[0], 'n') && CharIs(p[1], 'a') && CharIs(p[2], 'n')) {
        bits = AssembleBits(neg, infExp, quiet, isFloat); ec = 0; return (int)(p + 3 - first);
    }
    if (hex) {
        u64 hm = 0; int hbits = 0; long binexp = 0; bool seenDot = false, any = false;
        for (; p < last; ++p) {
            if (*p == '.') { if (seenDot) break; seenDot = true; continue; }
            int v = HexVal(*p); if (v < 0) break; any = true;
            if (hbits < 60) { hm = (hm << 4) | v; hbits += 4; if (seenDot) binexp -= 4; }
            else if (!seenDot && binexp < kExpAccumCap) binexp += 4;
        }
        if (!any) { ec = 22; return 0; }
        if (p < last && CharIs(*p, 'p')) {
            const char *ep = p + 1; bool en = false;
            if (ep < last && (*ep == '+' || *ep == '-')) { en = *ep == '-'; ++ep; }
            if (ep < last && *ep >= '0' && *ep <= '9') {
                long ev = 0;
                while (ep < last && *ep >= '0' && *ep <= '9') { if (ev < kExpAccumCap) ev = ev * 10 + (*ep - '0'); ++ep; }
                binexp += en ? -ev : ev; p = ep;
            }
        }
        if (hm == 0) { bits = AssembleBits(neg, 0, 0, isFloat); ec = 0; return (int)(p - first); }
        // Ф22a: route a definitely-out-of-range magnitude to result_out_of_range
        // BEFORE BigShl, which would otherwise shift binexp bits (binexp/32 words)
        // into a fixed-size BigInt. value = hm * 2^binexp, with 1 <= hm < 2^60.
        long maxBinExp = isFloat ? 130 : 1025;    // hm>=1 => |value| >= 2^binexp > MAX
        long minBinExp = isFloat ? -211 : -1136;  // |value| <= 2^(binexp+60) <= min_subnormal/2 => rounds to 0
        if (binexp >= maxBinExp || binexp <= minBinExp) {
            bits = AssembleBits(neg, 0, 0, isFloat); ec = 34; return (int)(p - first);
        }
        BigInt num; BigSetU64(num, hm); BigInt den; BigSetU64(den, 1);
        if (binexp >= 0) { BigInt t; BigShl(num, (int)binexp, t); num = t; } else { BigInt t; BigShl(den, (int)(-binexp), t); den = t; }
        u64 ef, mant; bool oor = false;
        RoundToIeee(num, den, P, isFloat ? -126 : -1022, isFloat ? 127 : 1023, ef, mant, oor);
        bits = AssembleBits(neg, ef, mant, isFloat); ec = oor ? 34 : 0; return (int)(p - first);
    }
    char dig[1300]; int ndig = 0; long E = 0; bool seenDot = false, any = false;
    for (; p < last; ++p) {
        if (*p == '.') { if (seenDot) break; seenDot = true; continue; }
        if (*p < '0' || *p > '9') break;
        any = true;
        if (ndig < 1290) { dig[ndig++] = *p; if (seenDot) --E; }
        else if (!seenDot && E < kExpAccumCap) ++E;
    }
    if (!any) { ec = 22; return 0; }
    const char *afterMant = p;
    if (mode != M_FIXED && p < last && CharIs(*p, 'e')) {
        const char *ep = p + 1; bool en = false; if (ep < last && (*ep == '+' || *ep == '-')) { en = *ep == '-'; ++ep; }
        if (ep < last && *ep >= '0' && *ep <= '9') { long ev = 0; while (ep < last && *ep >= '0' && *ep <= '9') { if (ev < kExpAccumCap) ev = ev * 10 + (*ep - '0'); ++ep; } E += en ? -ev : ev; p = ep; }
        else if (mode == M_SCI) { ec = 22; return 0; }
        else p = afterMant;
    } else if (mode == M_SCI) { ec = 22; return 0; }
    // Ф22a: route a definitely-out-of-range magnitude to result_out_of_range
    // BEFORE the 10^|E| scaling in DigitsToBits, which would otherwise grow a
    // fixed-size BigInt without bound. value = INT(dig) * 10^E; with nsig
    // leading-zero-stripped digits, 10^(nsig-1+E) <= |value| < 10^(nsig+E).
    {
        int lead = 0; while (lead < ndig && dig[lead] == '0') ++lead;
        int nsig = ndig - lead;
        // nsig == 0 ⇒ all-zero significand (value is exactly 0): no over/underflow
        // is possible, and DigitsToBits returns signed zero before it ever uses E,
        // so skipping the pre-clamp (and its narrowing (int)E) here is correct.
        if (nsig > 0) {
            long g_lo = (long)(nsig - 1) + E;      // lower bound on log10|value|
            long g_hi = (long)nsig + E;             // strict upper bound on log10|value|
            long maxExp10 = isFloat ? 39 : 309;     // |value| >= 10^maxExp10 => overflow
            long minExp10 = isFloat ? -46 : -324;   // |value| <  10^minExp10 => underflow to 0
            if (g_lo >= maxExp10 || g_hi <= minExp10) {
                bits = AssembleBits(neg, 0, 0, isFloat); ec = 34; return (int)(p - first);
            }
        }
    }
    bool oor = false; bits = DigitsToBits(neg, dig, ndig, (int)E, isFloat, oor);
    ec = oor ? 34 : 0; return (int)(p - first);
}
int FmtToParseMode(std::chars_format fmt, bool &hex)
{
    hex = fmt == std::chars_format::hex;
    if (fmt == std::chars_format::scientific) return M_SCI;
    if (fmt == std::chars_format::fixed) return M_FIXED;
    return M_GENERAL;
}

// ════════════════════════════════════════════════════════════════════════════
// long double (80-bit x87 extended). Ф27b. Own tableless Dragon4 shortest, a
// denominator-based precision renderer (never materializes M·5^-e2), and a
// P=64 parser with 128-bit transient quotients. Reuses the BigInt substrate.
// x87 layout: bytes 0-7 = 64-bit significand M (bit63 = explicit integer bit),
// bytes 8-9 = se (bit15 sign, bits0-14 = 15-bit biased exponent).
// ════════════════════════════════════════════════════════════════════════════
constexpr int kLdSigDigits = 11536;  // ⌈bits of M·5^16445⌉ in decimal (exact digits of the smallest subnormal)
constexpr int kLdIntDigits = 4944;   // decimal digits of the largest integer value (LDBL_MAX ≈ 10^4932)

void DecomposeLd(long double x, bool &sign, unsigned &exp, u64 &M, i32 &e2)
{
    unsigned __int128 bits = __builtin_bit_cast(unsigned __int128, x);
    M    = (u64)bits;
    unsigned se = (unsigned)(uint16_t)(bits >> 64);
    sign = se >> 15;
    exp  = se & 0x7FFF;
    e2   = (i32)(exp == 0 ? 1 : exp) - 16383 - 63;   // value = M · 2^e2
}

// Dragon4 (Steele & White FPP2 / Burger-Dubois free-format) shortest: emits the
// fewest decimal digits that round-trip. R,S,M+,M- are scaled by 2 (×4 at the
// lower boundary where the ulp halves); accept-bounds inclusive iff M is even.
void LdDragonShortest(u64 M, i32 e2, bool boundary, char *digits, int &ndig, int &e10)
{
    BigInt R, S, Mp, Mm, tmp;
    BigSetU64(R, M);
    bool even = (M & 1) == 0;
    if (e2 >= 0) {
        if (!boundary) { BigShl(R, e2 + 1, tmp); R = tmp; BigSetU64(S, 2);
                         BigSetU64(Mp, 1); BigShl(Mp, e2, tmp); Mp = tmp; Mm = Mp; }
        else { BigShl(R, e2 + 2, tmp); R = tmp; BigSetU64(S, 4);
               BigSetU64(Mp, 1); BigShl(Mp, e2 + 1, tmp); Mp = tmp;
               BigSetU64(Mm, 1); BigShl(Mm, e2, tmp); Mm = tmp; }
    } else {
        int ne = -e2;
        if (!boundary) { BigShl(R, 1, tmp); R = tmp; BigSetU64(S, 1); BigShl(S, ne + 1, tmp); S = tmp;
                         BigSetU64(Mp, 1); BigSetU64(Mm, 1); }
        else { BigShl(R, 2, tmp); R = tmp; BigSetU64(S, 1); BigShl(S, ne + 2, tmp); S = tmp;
               BigSetU64(Mp, 2); BigSetU64(Mm, 1); }
    }
    i32 bitDiff = BigBitLen(R) - BigBitLen(S);
    i32 k = (i32)(((int64_t)bitDiff * 30103) / 100000);
    if (k >= 0) { for (i32 i = 0; i < k; ++i) BigMulSmall(S, 10); }
    else { for (i32 i = 0; i < -k; ++i) { BigMulSmall(R, 10); BigMulSmall(Mp, 10); BigMulSmall(Mm, 10); } }
    for (;;) { BigInt s10 = S; BigMulSmall(s10, 10); if (BigCmpFull(R, s10) < 0) break; BigMulSmall(S, 10); ++k; }
    while (BigCmpFull(R, S) < 0) { BigMulSmall(R, 10); BigMulSmall(Mp, 10); BigMulSmall(Mm, 10); --k; }
    int decimal_point = k + 1;
    ndig = 0;
    for (;;) {
        int d = 0; while (BigCmpFull(R, S) >= 0) { BigSubFull(R, S); ++d; }
        bool inMinus = even ? (BigCmpFull(R, Mm) <= 0) : (BigCmpFull(R, Mm) < 0);
        BigInt sum; BigAddFull(R, Mp, sum); int cp = BigCmpFull(sum, S);
        bool inPlus = even ? (cp >= 0) : (cp > 0);
        if (!inMinus && !inPlus) {
            digits[ndig++] = (char)('0' + d);
            BigMulSmall(R, 10); BigMulSmall(Mp, 10); BigMulSmall(Mm, 10);
        } else if (inMinus && inPlus) {
            BigInt r2; BigShl(R, 1, r2); int c = BigCmpFull(r2, S);
            if (c > 0 || (c == 0 && (d & 1))) ++d;
            digits[ndig++] = (char)('0' + d); break;
        } else if (inPlus) { ++d; digits[ndig++] = (char)('0' + d); break; }
        else { digits[ndig++] = (char)('0' + d); break; }
    }
    if (digits[ndig - 1] > '9') {   // round-up carried the last digit past '9'
        int i = ndig - 1;
        for (;;) {
            if (digits[i] != ('9' + 1)) break;
            digits[i] = '0'; if (i == 0) { digits[0] = '1'; ++decimal_point; ndig = 1; break; }
            --i; ++digits[i];
        }
    }
    while (ndig > 1 && digits[ndig - 1] == '0') --ndig;
    e10 = decimal_point - ndig;
}

void WriteExpLd(OutBuf &o, i32 e)   // like WriteExp, but up to 4 exponent digits (LD reaches ±4932)
{
    o.put('e');
    if (e < 0) { o.put('-'); e = -e; } else o.put('+');
    if (e >= 1000) { o.put((char)('0' + e / 1000)); e %= 1000; o.put((char)('0' + e / 100)); e %= 100; }
    else if (e >= 100) { o.put((char)('0' + e / 100)); e %= 100; }
    o.put((char)('0' + e / 10));
    o.put((char)('0' + e % 10));
}
bool RoundUpArr(char *sig, int n)   // +1 ULP into sig[0..n); true if it carried out (result = 1e^…)
{
    int i = n - 1;
    for (;;) {
        if (sig[i] != '9') { ++sig[i]; return false; }
        sig[i] = '0'; if (i == 0) { sig[0] = '1'; return true; } --i;
    }
}
int BigEmitDecimal(BigInt a, char *out)   // full decimal of a (thousands of digits) — BigToDec's tmp[1200] is too small
{
    if (a.n == 0) { out[0] = '0'; return 1; }
    char rev[kLdIntDigits]; int n = 0;
    while (a.n > 0 && n < kLdIntDigits) rev[n++] = (char)('0' + BigDivModSmall(a, 10));
    for (int i = 0; i < n; ++i) out[i] = rev[n - 1 - i];
    return n;
}
int EmitExactIntLd(char *out, u64 M, i32 e2)   // round(M·2^e2) as decimal digits, ties→even
{
    if (e2 >= 0) { BigInt b; BigSetU64(b, M); BigInt s; BigShl(b, e2, s); return BigEmitDecimal(s, out); }
    int sh   = -e2;
    u64 q    = sh >= 64 ? 0 : (M >> sh);
    u64 rem  = sh >= 64 ? M : (M & ((1ull << sh) - 1));
    u64 half = (sh >= 1 && sh <= 64) ? (1ull << (sh - 1)) : 0;
    if (rem > half || (rem == half && (q & 1))) ++q;
    char tmp[24]; int n = 0; do { tmp[n++] = (char)('0' + q % 10); q /= 10; } while (q);
    for (int i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    return n;
}
i32 LdDecExp(u64 M, i32 e2)   // floor(log10(M·2^e2)) exactly
{
    BigInt num, den, t;
    BigSetU64(num, M);
    if (e2 >= 0) { BigShl(num, e2, t); num = t; BigSetU64(den, 1); }
    else { BigSetU64(den, 1); BigShl(den, -e2, t); den = t; }
    i32 d = (i32)(((int64_t)(BigBitLen(num) - BigBitLen(den)) * 30103) / 100000);
    if (d >= 0) { for (i32 i = 0; i < d; ++i) BigMulSmall(den, 10); }
    else { for (i32 i = 0; i < -d; ++i) BigMulSmall(num, 10); }
    for (;;) { BigInt d10 = den; BigMulSmall(d10, 10); if (BigCmpFull(num, d10) < 0) break; BigMulSmall(den, 10); ++d; }
    while (BigCmpFull(num, den) < 0) { BigMulSmall(num, 10); --d; }
    return d;
}
void LdGenSigDigits(u64 M, i32 e2, int wantSig, char *out, int cap, i32 &decExp, bool &roundTail);

// shortest digit string → fixed / scientific / general, matching EmitFp's selection.
void EmitFpLd(OutBuf &o, bool sign, const char *digits, int ndig, i32 e10, int mode, u64 M, i32 e2)
{
    i32 e_sci = e10 + ndig - 1;
    i32 ea = e_sci < 0 ? -e_sci : e_sci;
    int expDig = ea >= 1000 ? 4 : ea >= 100 ? 3 : 2;
    long sci_len = ndig + (ndig > 1 ? 1 : 0) + 2 + expDig;
    char intbuf[kLdIntDigits]; int intlen = 0;
    long fixed_len;
    if (e10 >= 0) { intlen = EmitExactIntLd(intbuf, M, e2); fixed_len = intlen; }
    else { i32 dot = ndig + e10; fixed_len = dot <= 0 ? (2 + (-(long)dot) + ndig) : (ndig + 1); }
    bool use_fixed;
    switch (mode) {
        case M_SCI:     use_fixed = false; break;
        case M_FIXED:   use_fixed = true; break;
        case M_GENERAL: use_fixed = (e_sci >= -4 && e_sci <= 5); break;
        default:        use_fixed = fixed_len <= sci_len; break;
    }
    if (sign) o.put('-');
    if (use_fixed) {
        if (e10 >= 0) o.run(intbuf, intlen);
        else {
            i32 dot = ndig + e10;
            if (dot <= 0) { o.put('0'); o.put('.'); o.zeros(-dot); o.run(digits, ndig); }
            else { o.run(digits, dot); o.put('.'); o.run(digits + dot, ndig - dot); }
        }
    } else {
        o.put(digits[0]);
        if (ndig > 1) { o.put('.'); o.run(digits + 1, ndig - 1); }
        WriteExpLd(o, e_sci);
    }
}
void FmtSciPLd(OutBuf &o, bool sign, u64 M, i32 e2, int p)
{
    char S[kLdSigDigits];
    int gen = (long)p + 1 < kLdSigDigits ? p + 1 : kLdSigDigits;
    i32 X; bool rt; LdGenSigDigits(M, e2, gen, S, kLdSigDigits, X, rt);
    if (rt && RoundUpArr(S, gen)) ++X;
    if (sign) o.put('-');
    o.put(S[0]);
    if (p > 0) { o.put('.'); int fromS = gen - 1 < p ? gen - 1 : p; o.run(S + 1, fromS); o.zeros((long)p - fromS); }
    WriteExpLd(o, X);
}
void FmtFixedPLd(OutBuf &o, bool sign, u64 M, i32 e2, int p)
{
    i32 decExp = LdDecExp(M, e2);
    long want = (long)decExp + p + 1;
    if (sign) o.put('-');
    if (want <= 0) {   // value < 10^-p: rounds to 0 or a single 1 at the 10^-p place
        BigInt num, den, t; BigSetU64(num, M); BigSetU64(den, 1);
        int sh = e2 + 1;
        if (sh >= 0) { BigShl(num, sh, t); num = t; } else { BigShl(den, -sh, t); den = t; }
        for (int i = 0; i < p; ++i) BigMulSmall(num, 10);
        bool up = BigCmpFull(num, den) > 0;   // 2·value > 10^-p (exact half → even → down)
        if (p == 0) { o.put(up ? '1' : '0'); return; }
        o.put('0'); o.put('.');
        if (up) { o.zeros((long)p - 1); o.put('1'); } else o.zeros(p);
        return;
    }
    char S[kLdSigDigits];
    int gen = want < kLdSigDigits ? (int)want : kLdSigDigits;
    i32 X; bool rt; LdGenSigDigits(M, e2, gen, S, kLdSigDigits, X, rt);
    if (rt && RoundUpArr(S, gen)) ++X;
    int intLen = X >= 0 ? X + 1 : 0;
    if (intLen <= 0) o.put('0');
    else if (intLen <= gen) o.run(S, intLen);
    else { o.run(S, gen); o.zeros((long)intLen - gen); }
    if (p > 0) {
        o.put('.');
        if (X >= 0) {
            int fracStart = X + 1, avail = gen - fracStart; if (avail < 0) avail = 0;
            int emit = avail < p ? avail : p;
            o.run(S + fracStart, emit); o.zeros((long)p - emit);
        } else {
            long lead = -(long)X - 1;
            if (lead >= p) o.zeros(p);
            else { o.zeros(lead); int emit = gen < (p - lead) ? gen : (int)(p - lead);
                   o.run(S, emit); o.zeros((long)p - lead - emit); }
        }
    }
}
void FmtGeneralPLd(OutBuf &o, bool sign, u64 M, i32 e2, int p)
{
    int P = p < 1 ? 1 : p;
    if (P > kLdSigDigits) P = kLdSigDigits;
    char S[kLdSigDigits];
    i32 X; bool rt; LdGenSigDigits(M, e2, P, S, kLdSigDigits, X, rt);
    int sig = P;
    if (rt && RoundUpArr(S, sig)) ++X;
    if (sign) o.put('-');
    if (X < -4 || X >= P) {
        int fracEnd = sig; while (fracEnd > 1 && S[fracEnd - 1] == '0') --fracEnd;
        o.put(S[0]); if (fracEnd > 1) { o.put('.'); o.run(S + 1, fracEnd - 1); }
        WriteExpLd(o, X);
    } else if (X >= 0) {
        int intLen = X + 1;
        if (intLen >= sig) { o.run(S, sig); o.zeros((long)intLen - sig); }
        else { int fracEnd = sig; while (fracEnd > intLen && S[fracEnd - 1] == '0') --fracEnd;
               o.run(S, intLen);
               if (fracEnd > intLen) { o.put('.'); o.run(S + intLen, fracEnd - intLen); } }
    } else {
        int fracEnd = sig; while (fracEnd > 0 && S[fracEnd - 1] == '0') --fracEnd;
        o.put('0');
        if (fracEnd > 0) { o.put('.'); o.zeros(-(long)X - 1); o.run(S, fracEnd); }
    }
}
// value = M·2^e2 = num/den. Streams up to min(wantSig,cap) significant digits from
// the first nonzero (MSB-first) into out; decExp = power of ten of out[0]; roundTail
// = round-half-even decision at digit #(wantSig+1). Big-ints stay ≤ ~514 words: it
// never materializes M·5^-e2, only the intrinsic 2^-e2 denominator.
void LdGenSigDigits(u64 M, i32 e2, int wantSig, char *out, int cap, i32 &decExp, bool &roundTail)
{
    roundTail = false;
    BigInt num, den, t;
    BigSetU64(num, M);
    if (e2 >= 0) { BigShl(num, e2, t); num = t; BigSetU64(den, 1); }
    else { BigSetU64(den, 1); BigShl(den, -e2, t); den = t; }
    i32 bitDiff = BigBitLen(num) - BigBitLen(den);
    decExp = (i32)(((int64_t)bitDiff * 30103) / 100000);
    if (decExp >= 0) { for (i32 i = 0; i < decExp; ++i) BigMulSmall(den, 10); }
    else { for (i32 i = 0; i < -decExp; ++i) BigMulSmall(num, 10); }
    for (;;) { BigInt d10 = den; BigMulSmall(d10, 10); if (BigCmpFull(num, d10) < 0) break; BigMulSmall(den, 10); ++decExp; }
    while (BigCmpFull(num, den) < 0) { BigMulSmall(num, 10); --decExp; }
    int want = wantSig < cap ? wantSig : cap;
    if (want < 1) want = 1;
    int k = 0;
    for (; k < want; ++k) {
        if (num.n == 0) { for (int j = k; j < want; ++j) out[j] = '0'; break; }
        int d = 0; while (BigCmpFull(num, den) >= 0) { BigSubFull(num, den); ++d; }
        out[k] = (char)('0' + d);
        BigMulSmall(num, 10);
    }
    int dnext = 0; while (BigCmpFull(num, den) >= 0) { BigSubFull(num, den); ++dnext; }
    bool tail = num.n != 0;
    if (dnext > 5) roundTail = true;
    else if (dnext == 5) roundTail = tail || ((out[want - 1] - '0') & 1);
}
void RenderLd(OutBuf &o, long double x, int mode, bool hex)
{
    bool sign; unsigned exp; u64 M; i32 e2; DecomposeLd(x, sign, exp, M, e2);
    if (exp == 0x7FFF) {   // x87 inf = mant == 0x8000000000000000; nan otherwise
        if (sign) o.put('-');
        o.run(M != 0x8000000000000000ull ? "nan" : "inf", 3); return;
    }
    if (exp == 0 && M == 0) {
        char b[24];
        int len = hex ? EmitHex(b, sign, 0, 16, 0, 0) : EmitFp(b, sign, 0, 0, 0, 0, mode);
        o.run(b, len); return;
    }
    if (hex) {
        char b[48]; i32 binExp; u64 frac;
        if (exp == 0) {   // subnormal: normalize to a leading 1
            int h = 62; while (h >= 0 && !((M >> h) & 1)) --h;
            binExp = h - 16445; frac = h ? ((M & ((((u64)1) << h) - 1)) << (64 - h)) : 0;
        } else { binExp = (i32)exp - 16383; frac = (M & 0x7FFFFFFFFFFFFFFFull) << 1; }
        int len = EmitHex(b, sign, frac, 16, binExp, 1);
        o.run(b, len); return;
    }
    bool boundary = (M == 0x8000000000000000ull) && (exp > 1);
    char digits[24]; int ndig, e10;
    LdDragonShortest(M, e2, boundary, digits, ndig, e10);
    EmitFpLd(o, sign, digits, ndig, e10, mode, M, e2);
}

// ── from_chars (decimal/hex → 80-bit, correctly rounded) ────────────────────
u128 AssembleBitsLd(bool neg, unsigned expField, u64 mant)
{
    return ((u128)(((neg ? 0x8000u : 0u) | expField)) << 64) | mant;
}
// P=64, emin=-16382, emax=16383. num/den is already exact (the guard digit/bit
// in DigitsToBitsLd/ParseFpLd folds the dropped tail), so ties→even is exact.
void RoundToIeeeLd(const BigInt &num, const BigInt &den, u64 &mant, unsigned &expField, bool &oor)
{
    oor = false;
    i32 e2 = BigBitLen(num) - BigBitLen(den) - 1;
    u128 q = RoundedQ128(num, den, 63 - e2);
    int g = 0;
    while (q >= ((u128)1 << 64)) { ++e2; q = RoundedQ128(num, den, 63 - e2); if (++g > 6) break; }
    while (q < ((u128)1 << 63)) { --e2; q = RoundedQ128(num, den, 63 - e2); if (++g > 6) break; }
    if (e2 > 16383) { oor = true; expField = 0x7FFF; mant = 0; return; }
    if (e2 >= -16382) { expField = (unsigned)(e2 + 16383); mant = (u64)q; return; }  // keep the explicit J bit
    u128 qs = RoundedQ128(num, den, 16445);
    if (qs == 0) { oor = true; expField = 0; mant = 0; return; }   // underflow to zero
    if (qs >= ((u128)1 << 63)) { expField = 1; mant = (u64)qs; }
    else { expField = 0; mant = (u64)qs; }
}
// digits MSB-first ('0'..'9'), value = INT(dig[0..ndig)) · 10^E, sticky = whether
// anything nonzero was dropped past the kept window. Ф27b REFINEMENT 1: the sticky
// enters as a low GUARD DIGIT (never BigInc), so a just-below-midpoint input with a
// >1290-digit tail stays strictly below and rounds down.
void DigitsToBitsLd(bool neg, const char *dig, int ndig, long E, bool sticky,
                    u64 &mant, unsigned &expField, bool &oor)
{
    (void)neg;
    int i0 = 0; while (i0 < ndig && dig[i0] == '0') ++i0;
    int hi = ndig; while (hi > i0 && dig[hi - 1] == '0') { --hi; ++E; }
    if (hi - i0 == 0 && !sticky) { expField = 0; mant = 0; oor = false; return; }
    BigInt num; BigSetU64(num, 0);
    for (int i = i0; i < hi; ++i) {
        BigMulSmall(num, 10);
        u64 carry = (u64)(dig[i] - '0');
        for (int k = 0; k < num.n && carry; ++k) { u64 s = (u64)num.w[k] + carry; num.w[k] = (u32)s; carry = s >> 32; }
        if (carry && num.n < kBigWords) num.w[num.n++] = (u32)carry;
    }
    // REFINEMENT 3 (for the end-of-phase host oracle): random sampling will NOT hit the
    // hard cases — the oracle must CONSTRUCT subnormal binary-midpoints ±ε with >1290
    // decimal digits and hex inputs with >64 significant bits to exercise this guard.
    BigMulSmall(num, 10); if (sticky) { if (num.n == 0) num.n = 1; num.w[0] |= 1; } E -= 1;   // guard digit
    BigInt den; BigSetU64(den, 1);
    if (E >= 0) { for (long i = 0; i < E; ++i) BigMulSmall(num, 10); }
    else { for (long i = 0; i < -E; ++i) BigMulSmall(den, 10); }
    RoundToIeeeLd(num, den, mant, expField, oor);
}
// ── heap big-integer (runtime capacity) for from_chars(long double) ──────────
// Used ONLY when a decimal input carries MORE than 1290 significant digits, so an
// EXACT subnormal binary midpoint (up to ~11515 digits) rounds half-even instead
// of being folded into the sticky bit and rounded up. float/double and the
// ≤1290-digit long-double fast path never allocate — they stay on the stack.
// Bounded: at most kLdSigDigits kept digits ⇒ ≤ ~1720 words, so kLdHeapWords is a
// hard ceiling the ops saturate at (proven, never reached); every allocation is
// freed on every exit path; malloc failure degrades to the stack path.
constexpr int kLdHeapWords = 1920;
struct HBig { u32 *w; int n; int cap; };
void HBigSetU64(HBig &a, u64 v) {
    a.w[0] = (u32)v; a.w[1] = (u32)(v >> 32);
    a.n = a.w[1] ? 2 : (a.w[0] ? 1 : 0);
}
void HBigAssign(HBig &d, const HBig &s) { for (int i = 0; i < s.n; ++i) d.w[i] = s.w[i]; d.n = s.n; }
void HBigMulSmall(HBig &a, u32 m) {
    u64 carry = 0;
    for (int i = 0; i < a.n; ++i) { u64 p = (u64)a.w[i] * m + carry; a.w[i] = (u32)p; carry = p >> 32; }
    while (carry && a.n < a.cap) { a.w[a.n++] = (u32)carry; carry >>= 32; }
}
int HBigBitLen(const HBig &a) {
    if (a.n == 0) return 0;
    int bits = (a.n - 1) * 32; u32 hi = a.w[a.n - 1];
    while (hi) { ++bits; hi >>= 1; }
    return bits;
}
void HBigShl(const HBig &a, int s, HBig &out) {
    int wsh = s / 32, bsh = s % 32;
    for (int i = 0; i < out.cap; ++i) out.w[i] = 0;
    for (int i = 0; i < a.n; ++i) {
        u64 v = (u64)a.w[i] << bsh; int lo = i + wsh, hi = lo + 1;
        if (lo >= 0 && lo < out.cap) out.w[lo] |= (u32)v;          // saturate at cap:
        if (hi >= 0 && hi < out.cap) out.w[hi] |= (u32)(v >> 32);  // can never write OOB
    }
    out.n = a.n + wsh + 1; if (out.n > out.cap) out.n = out.cap;
    while (out.n > 0 && out.w[out.n - 1] == 0) --out.n;
}
void HBigShl1(HBig &a) {  // a <<= 1, in place
    u32 carry = 0;
    for (int i = 0; i < a.n; ++i) { u32 nc = a.w[i] >> 31; a.w[i] = (a.w[i] << 1) | carry; carry = nc; }
    if (carry && a.n < a.cap) a.w[a.n++] = carry;
}
int HBigCmp(const HBig &a, const HBig &b) {
    if (a.n != b.n) return a.n > b.n ? 1 : -1;
    for (int i = a.n - 1; i >= 0; --i) if (a.w[i] != b.w[i]) return a.w[i] > b.w[i] ? 1 : -1;
    return 0;
}
void HBigSub(HBig &a, const HBig &b) {  // a -= b, requires a >= b
    u64 borrow = 0;
    for (int i = 0; i < a.n; ++i) { u64 d = i < b.n ? b.w[i] : 0, c = (u64)a.w[i] - d - borrow; a.w[i] = (u32)c; borrow = (c >> 63) & 1; }
    while (a.n > 0 && a.w[a.n - 1] == 0) --a.n;
}
void HBigDivMod128(const HBig &num, const HBig &den, u128 &q, HBig &rem) {  // low128 q + rem
    rem.n = 0; q = 0;
    for (int bit = HBigBitLen(num) - 1; bit >= 0; --bit) {
        HBigShl1(rem);   // no-op when rem.n==0 and does NOT touch rem.w[0] (unlike the
        // zero-filling stack BigShl), so on the empty→nonempty step below rem.w[0]
        // must be explicitly zeroed — otherwise the |=1 ORs into a stale heap word.
        if ((num.w[bit / 32] >> (bit % 32)) & 1) { if (rem.n == 0) { rem.w[0] = 0; rem.n = 1; } rem.w[0] |= 1; }
        if (HBigCmp(rem, den) >= 0) { HBigSub(rem, den); if (bit < 128) q |= ((u128)1 << bit); }
    }
}
u128 HBigRoundedQ128(const HBig &num, const HBig &den, int shift, HBig &sn, HBig &sd, HBig &rem, HBig &rem2) {
    if (shift >= 0) { HBigShl(num, shift, sn); HBigAssign(sd, den); }
    else { HBigAssign(sn, num); HBigShl(den, -shift, sd); }
    u128 q; HBigDivMod128(sn, sd, q, rem);
    HBigAssign(rem2, rem); HBigShl1(rem2); int c = HBigCmp(rem2, sd);
    if (c > 0 || (c == 0 && (u64)(q & 1))) ++q;
    return q;
}
// Mirror of RoundToIeeeLd on heap big-ints (P=64, emin=-16382, emax=16383).
void HBigRoundToIeeeLd(const HBig &num, const HBig &den, HBig &sn, HBig &sd, HBig &rem, HBig &rem2,
                       u64 &mant, unsigned &expField, bool &oor) {
    oor = false;
    i32 e2 = HBigBitLen(num) - HBigBitLen(den) - 1;
    u128 q = HBigRoundedQ128(num, den, 63 - e2, sn, sd, rem, rem2);
    int g = 0;
    while (q >= ((u128)1 << 64)) { ++e2; q = HBigRoundedQ128(num, den, 63 - e2, sn, sd, rem, rem2); if (++g > 6) break; }
    while (q < ((u128)1 << 63)) { --e2; q = HBigRoundedQ128(num, den, 63 - e2, sn, sd, rem, rem2); if (++g > 6) break; }
    if (e2 > 16383) { oor = true; expField = 0x7FFF; mant = 0; return; }
    if (e2 >= -16382) { expField = (unsigned)(e2 + 16383); mant = (u64)q; return; }
    u128 qs = HBigRoundedQ128(num, den, 16445, sn, sd, rem, rem2);
    if (qs == 0) { oor = true; expField = 0; mant = 0; return; }
    if (qs >= ((u128)1 << 63)) { expField = 1; mant = (u64)qs; }
    else { expField = 0; mant = (u64)qs; }
}
// Round INT(dig[0..ndig))·10^E to 80-bit exactly via heap big-ints (guard digit
// folds the sticky tail, identical to DigitsToBitsLd). Returns false iff the
// scratch allocation failed (caller falls back to the sticky stack path).
bool DigitsToBitsLdHeap(const char *dig, int ndig, long E, bool sticky,
                        u64 &mant, unsigned &expField, bool &oor) {
    u32 *blk = (u32 *)_malloc_impl(sizeof(u32) * 6 * kLdHeapWords);
    if (!blk) return false;
    HBig num {blk + 0 * kLdHeapWords, 0, kLdHeapWords};
    HBig den {blk + 1 * kLdHeapWords, 0, kLdHeapWords};
    HBig sn  {blk + 2 * kLdHeapWords, 0, kLdHeapWords};
    HBig sd  {blk + 3 * kLdHeapWords, 0, kLdHeapWords};
    HBig rem {blk + 4 * kLdHeapWords, 0, kLdHeapWords};
    HBig rem2{blk + 5 * kLdHeapWords, 0, kLdHeapWords};
    int i0 = 0; while (i0 < ndig && dig[i0] == '0') ++i0;
    int hi = ndig; while (hi > i0 && dig[hi - 1] == '0') { --hi; ++E; }
    if (hi - i0 == 0 && !sticky) { expField = 0; mant = 0; oor = false; free(blk); return true; }
    HBigSetU64(num, 0);
    for (int i = i0; i < hi; ++i) {
        HBigMulSmall(num, 10);
        u64 carry = (u64)(dig[i] - '0');
        for (int k = 0; k < num.n && carry; ++k) { u64 s = (u64)num.w[k] + carry; num.w[k] = (u32)s; carry = s >> 32; }
        if (carry && num.n < num.cap) num.w[num.n++] = (u32)carry;
    }
    HBigMulSmall(num, 10); if (sticky) { if (num.n == 0) num.n = 1; num.w[0] |= 1; } E -= 1;  // guard digit
    HBigSetU64(den, 1);
    if (E >= 0) { for (long i = 0; i < E; ++i) HBigMulSmall(num, 10); }
    else { for (long i = 0; i < -E; ++i) HBigMulSmall(den, 10); }
    HBigRoundToIeeeLd(num, den, sn, sd, rem, rem2, mant, expField, oor);
    free(blk);
    return true;
}
// Exact heap path for a decimal mantissa [ms,me) that carries >1290 significant
// digits. Re-scans keeping up to kLdSigDigits digits (every exact midpoint
// terminates within that) and folds the rest into sticky. Returns false only if
// a buffer could not be allocated (caller falls back to the sticky stack path).
bool ParseLdDecimalHeap(const char *ms, const char *me, long expVal, bool neg, u128 &bits, int &ec)
{
    char *dig = (char *)_malloc_impl((__SIZE_TYPE__)kLdSigDigits);
    if (!dig) return false;
    int ndig = 0; long E = 0; bool seenDot = false, sticky = false, seenNZ = false;
    for (const char *q = ms; q < me; ++q) {
        if (*q == '.') { seenDot = true; continue; }
        if (*q < '0' || *q > '9') break;
        if (!seenNZ && *q == '0') { if (seenDot) --E; continue; }
        seenNZ = true;
        if (ndig < kLdSigDigits) { dig[ndig++] = *q; if (seenDot) --E; }
        else { if (*q != '0') sticky = true; if (!seenDot && E < (1L << 40)) ++E; }
    }
    E += expVal;
    if (!seenNZ) { bits = AssembleBitsLd(neg, 0, 0); ec = 0; free(dig); return true; }
    { long g_lo = (long)(ndig - 1) + E, g_hi = (long)ndig + E;
      if (g_lo >= 4933 || g_hi <= -4951) { bits = AssembleBitsLd(neg, 0, 0); ec = 34; free(dig); return true; } }
    u64 mant; unsigned ef; bool oor = false;
    bool ok = DigitsToBitsLdHeap(dig, ndig, E, sticky, mant, ef, oor);
    free(dig);
    if (!ok) return false;
    bits = AssembleBitsLd(neg, ef, mant); ec = oor ? 34 : 0;
    return true;
}
// Parse a long double; returns consumed length, fills bits (u128), ec (0/22/34).
int ParseFpLd(const char *first, const char *last, int mode, bool hex, u128 &bits, int &ec)
{
    constexpr long kExpAccumCap = 1L << 40;
    const char *p = first; bool neg = false;
    if (p < last && *p == '-') { neg = true; ++p; }
    if (p < last && CharIs(*p, 'i') && last - p >= 3 && CharIs(p[0], 'i') && CharIs(p[1], 'n') && CharIs(p[2], 'f')) {
        const char *q = p + 3;
        if (last - p >= 8 && CharIs(p[3], 'i') && CharIs(p[4], 'n') && CharIs(p[5], 'i') && CharIs(p[6], 't') && CharIs(p[7], 'y')) q = p + 8;
        bits = AssembleBitsLd(neg, 0x7FFF, 0x8000000000000000ull); ec = 0; return (int)(q - first);
    }
    if (p < last && CharIs(*p, 'n') && last - p >= 3 && CharIs(p[0], 'n') && CharIs(p[1], 'a') && CharIs(p[2], 'n')) {
        bits = AssembleBitsLd(neg, 0x7FFF, 0xC000000000000000ull); ec = 0; return (int)(p + 3 - first);
    }
    if (hex) {
        u128 hm = 0; int hbits = 0; long binexp = 0;
        bool seenDot = false, any = false, sticky = false, seenNZ = false;
        for (; p < last; ++p) {
            if (*p == '.') { if (seenDot) break; seenDot = true; continue; }
            int v = HexVal(*p); if (v < 0) break; any = true;
            if (!seenNZ && v == 0) { if (seenDot) binexp -= 4; continue; }   // skip leading zero nibbles
            seenNZ = true;
            // REFINEMENT 2: capture ≥64 significant bits (here 120) + guard, rest → sticky
            if (hbits < 120) { hm = (hm << 4) | (unsigned)v; hbits += 4; if (seenDot) binexp -= 4; }
            else { if (v != 0) sticky = true; if (!seenDot && binexp < kExpAccumCap) binexp += 4; }
        }
        if (!any) { ec = 22; return 0; }
        if (p < last && CharIs(*p, 'p')) {
            const char *ep = p + 1; bool en = false;
            if (ep < last && (*ep == '+' || *ep == '-')) { en = *ep == '-'; ++ep; }
            if (ep < last && *ep >= '0' && *ep <= '9') {
                long ev = 0;
                while (ep < last && *ep >= '0' && *ep <= '9') { if (ev < kExpAccumCap) ev = ev * 10 + (*ep - '0'); ++ep; }
                binexp += en ? -ev : ev; p = ep;
            }
        }
        if (!seenNZ) { bits = AssembleBitsLd(neg, 0, 0); ec = 0; return (int)(p - first); }
        // pre-clamp (value = hm·2^binexp, hm has ≤120 significant bits): definite over/underflow
        if (binexp >= 16384 || binexp <= -16566) { bits = AssembleBitsLd(neg, 0, 0); ec = 34; return (int)(p - first); }
        hm = (hm << 1) | (sticky ? 1u : 0u); binexp -= 1;   // REFINEMENT 1: guard bit
        BigInt num; BigSetU128(num, hm); BigInt den; BigSetU64(den, 1); BigInt t;
        if (binexp >= 0) { BigShl(num, (int)binexp, t); num = t; } else { BigShl(den, (int)(-binexp), t); den = t; }
        u64 mant; unsigned ef; bool oor = false;
        RoundToIeeeLd(num, den, mant, ef, oor);
        bits = AssembleBitsLd(neg, ef, mant); ec = oor ? 34 : 0; return (int)(p - first);
    }
    // decimal. Leading zeros are skipped (not stored) so the 1290-digit window holds
    // significant digits — otherwise an LD subnormal like 0.<1290 zeros>… mis-rounds.
    char dig[1300]; int ndig = 0; long E = 0;
    bool seenDot = false, any = false, sticky = false, seenNZ = false, over1290 = false;
    const char *mantStart = p;   // replayed by the >1290-digit exact heap re-scan
    for (; p < last; ++p) {
        if (*p == '.') { if (seenDot) break; seenDot = true; continue; }
        if (*p < '0' || *p > '9') break;
        any = true;
        if (!seenNZ && *p == '0') { if (seenDot) --E; continue; }
        seenNZ = true;
        if (ndig < 1290) { dig[ndig++] = *p; if (seenDot) --E; }
        else { if (*p != '0') sticky = true; if (!seenDot && E < kExpAccumCap) ++E; over1290 = true; }
    }
    if (!any) { ec = 22; return 0; }
    const char *afterMant = p;
    long expVal = 0;   // signed exponent suffix, replayed by the heap re-scan
    if (mode != M_FIXED && p < last && CharIs(*p, 'e')) {
        const char *ep = p + 1; bool en = false; if (ep < last && (*ep == '+' || *ep == '-')) { en = *ep == '-'; ++ep; }
        if (ep < last && *ep >= '0' && *ep <= '9') { long ev = 0; while (ep < last && *ep >= '0' && *ep <= '9') { if (ev < kExpAccumCap) ev = ev * 10 + (*ep - '0'); ++ep; } expVal = en ? -ev : ev; E += expVal; p = ep; }
        else if (mode == M_SCI) { ec = 22; return 0; }
        else p = afterMant;
    } else if (mode == M_SCI) { ec = 22; return 0; }
    if (!seenNZ) { bits = AssembleBitsLd(neg, 0, 0); ec = 0; return (int)(p - first); }   // signed zero
    // pre-clamp (value = INT(dig)·10^E, nsig = ndig significant digits): 10^(nsig-1+E) ≤ |v| < 10^(nsig+E)
    {
        long g_lo = (long)(ndig - 1) + E, g_hi = (long)ndig + E;
        if (g_lo >= 4933 || g_hi <= -4951) { bits = AssembleBitsLd(neg, 0, 0); ec = 34; return (int)(p - first); }
    }
    // >1290 significant digits: an exact subnormal midpoint needs every digit, so
    // round it on the heap. Falls through to the stack path if allocation fails.
    if (over1290) {
        u128 hbits; int hec;
        if (ParseLdDecimalHeap(mantStart, afterMant, expVal, neg, hbits, hec)) {
            bits = hbits; ec = hec; return (int)(p - first);
        }
    }
    u64 mant; unsigned ef; bool oor = false;
    DigitsToBitsLd(neg, dig, ndig, E, sticky, mant, ef, oor);
    bits = AssembleBitsLd(neg, ef, mant); ec = oor ? 34 : 0; return (int)(p - first);
}
void RenderPrecLd(OutBuf &o, long double x, int mode, bool hex, int prec)
{
    bool sign; unsigned exp; u64 M; i32 e2; DecomposeLd(x, sign, exp, M, e2);
    if (exp == 0x7FFF) { if (sign) o.put('-'); o.run(M != 0x8000000000000000ull ? "nan" : "inf", 3); return; }
    if (hex) {
        int lead; i32 binExp; u64 frac;
        if (exp == 0 && M == 0) { lead = 0; binExp = 0; frac = 0; }
        else { lead = (int)((M >> 63) & 1); binExp = exp ? (i32)exp - 16383 : -16382; frac = (M & 0x7FFFFFFFFFFFFFFFull) << 1; }
        FmtHexP(o, sign, lead, binExp, frac, 64, prec); return;
    }
    if (exp == 0 && M == 0) { FmtZeroP(o, sign, mode, prec); return; }
    if (mode == M_FIXED) FmtFixedPLd(o, sign, M, e2, prec);
    else if (mode == M_SCI) FmtSciPLd(o, sign, M, e2, prec);
    else FmtGeneralPLd(o, sign, M, e2, prec);
}

} // namespace

namespace std {

to_chars_result to_chars(char *first, char *last, float value)
{
    EnsureTables();
    char buf[64]; int len = RenderFloat(buf, value, M_SHORTEST, false);
    return Commit(first, last, buf, len);
}
to_chars_result to_chars(char *first, char *last, double value)
{
    EnsureTables();
    char buf[350]; int len = RenderDouble(buf, value, M_SHORTEST, false);
    return Commit(first, last, buf, len);
}
to_chars_result to_chars(char *first, char *last, float value, chars_format fmt)
{
    EnsureTables();
    bool hex; int mode = FmtToMode(fmt, hex);
    char buf[64]; int len = RenderFloat(buf, value, mode, hex);
    return Commit(first, last, buf, len);
}
to_chars_result to_chars(char *first, char *last, double value, chars_format fmt)
{
    EnsureTables();
    bool hex; int mode = FmtToMode(fmt, hex);
    char buf[350]; int len = RenderDouble(buf, value, mode, hex);
    return Commit(first, last, buf, len);
}

// 5-arg precision overloads. The scratch bounds the practical precision
// (~1900 chars: sign + up to ~309 integer digits + point + fractional);
// outputs beyond it are reported as value_too_large by Commit.
to_chars_result to_chars(char *first, char *last, float value, chars_format fmt, int precision)
{
    bool hex; int mode = FmtToMode(fmt, hex);
    if (precision < 0) precision = 0;
    OutBuf o{first, last};
    RenderPrecFloat(o, value, mode, hex, precision);
    return o.of ? to_chars_result{last, errc::value_too_large} : to_chars_result{o.p, errc{}};
}
to_chars_result to_chars(char *first, char *last, double value, chars_format fmt, int precision)
{
    bool hex; int mode = FmtToMode(fmt, hex);
    if (precision < 0) precision = 0;
    OutBuf o{first, last};
    RenderPrecDouble(o, value, mode, hex, precision);
    return o.of ? to_chars_result{last, errc::value_too_large} : to_chars_result{o.p, errc{}};
}

// ── from_chars (floating-point) ─────────────────────────────────────────────
// value is written only on success; left unmodified on invalid_argument /
// result_out_of_range, per [charconv.from.chars].
static errc ParseEc(int ec)
{
    return ec == 22 ? errc::invalid_argument
                    : ec == 34 ? errc::result_out_of_range : errc{};
}
from_chars_result from_chars(const char *first, const char *last, float &value, chars_format fmt)
{
    bool hex; int mode = FmtToParseMode(fmt, hex);
    u64 bits; int ec; int n = ParseFp(first, last, mode, hex, true, bits, ec);
    if (ec == 0) value = __builtin_bit_cast(float, (uint32_t)bits);
    return {first + n, ParseEc(ec)};
}
from_chars_result from_chars(const char *first, const char *last, double &value, chars_format fmt)
{
    bool hex; int mode = FmtToParseMode(fmt, hex);
    u64 bits; int ec; int n = ParseFp(first, last, mode, hex, false, bits, ec);
    if (ec == 0) value = __builtin_bit_cast(double, bits);
    return {first + n, ParseEc(ec)};
}

// ── long double (Ф27b) ──────────────────────────────────────────────────────
// Tableless Dragon4 / big-int precision renderer — no Ryu table dependency, so
// no EnsureTables(). Everything writes through OutBuf into [first, last).
to_chars_result to_chars(char *first, char *last, long double value)
{
    OutBuf o{first, last};
    RenderLd(o, value, M_SHORTEST, false);
    return o.of ? to_chars_result{last, errc::value_too_large} : to_chars_result{o.p, errc{}};
}
to_chars_result to_chars(char *first, char *last, long double value, chars_format fmt)
{
    bool hex; int mode = FmtToMode(fmt, hex);
    OutBuf o{first, last};
    RenderLd(o, value, mode, hex);
    return o.of ? to_chars_result{last, errc::value_too_large} : to_chars_result{o.p, errc{}};
}
to_chars_result to_chars(char *first, char *last, long double value, chars_format fmt, int precision)
{
    bool hex; int mode = FmtToMode(fmt, hex);
    if (precision < 0) precision = 0;
    OutBuf o{first, last};
    RenderPrecLd(o, value, mode, hex, precision);
    return o.of ? to_chars_result{last, errc::value_too_large} : to_chars_result{o.p, errc{}};
}
from_chars_result from_chars(const char *first, const char *last, long double &value, chars_format fmt)
{
    bool hex; int mode = FmtToParseMode(fmt, hex);
    u128 bits; int ec; int n = ParseFpLd(first, last, mode, hex, bits, ec);
    if (ec == 0) value = __builtin_bit_cast(long double, bits);
    return {first + n, ParseEc(ec)};
}

} // namespace std
