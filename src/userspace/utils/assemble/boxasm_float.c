#include "boxasm_float.h"

#include "box/string.h"

/* Enough decimal digits that dropping the rest, remembered as one sticky
 * bit, can never move a rounding: a point halfway between two binary64
 * values has at most 767 significant digits. */
#define BOXASM_FLOAT_DIGITS 800u

/* Decimal magnitudes past which every width overflows, or rounds to zero. */
#define BOXASM_FLOAT_LARGEST 310
#define BOXASM_FLOAT_SMALLEST (-330)

/* 4480 bits: a divisor of 10^1130 with a quotient of 57 bits above it. */
#define BOXASM_BIG_LIMBS 140u

typedef struct {
    uint32_t Limb[BOXASM_BIG_LIMBS];
    uint32_t Count;      /* limbs in use, the top one not zero */
} BoxAsmBig;

typedef struct {
    uint8_t Precision;   /* significand bits, the hidden one counted */
    int16_t MinExponent; /* of the smallest normal value */
    int16_t Bias;        /* also the largest exponent */
} BoxAsmFloatFormat;

static const BoxAsmFloatFormat BoxAsmBinary32 = { 24, -126, 127 };
static const BoxAsmFloatFormat BoxAsmBinary64 = { 53, -1022, 1023 };

static void BoxAsmBigTrim(BoxAsmBig *A)
{
    while (A->Count != 0 && A->Limb[A->Count - 1u] == 0)
        A->Count--;
}

static void BoxAsmBigSet(BoxAsmBig *A, uint32_t Value)
{
    A->Count = Value != 0;
    A->Limb[0] = Value;
}

/* A * Factor + Add; -1 past the capacity. */
static int BoxAsmBigMultiplyAdd(BoxAsmBig *A, uint32_t Factor, uint32_t Add)
{
    uint64_t Carry = Add;
    uint32_t Index;

    for (Index = 0; Index < A->Count; Index++) {
        uint64_t Product = (uint64_t)A->Limb[Index] * Factor + Carry;

        A->Limb[Index] = (uint32_t)Product;
        Carry = Product >> 32;
    }
    if (Carry != 0) {
        if (A->Count == BOXASM_BIG_LIMBS)
            return -1;
        A->Limb[A->Count++] = (uint32_t)Carry;
    }
    return 0;
}

static int BoxAsmBigTimesTen(BoxAsmBig *A, uint32_t Times)
{
    while (Times-- > 0)
        if (BoxAsmBigMultiplyAdd(A, 10u, 0) != 0)
            return -1;
    return 0;
}

static int BoxAsmBigShiftLeft(BoxAsmBig *A, uint32_t Bits)
{
    uint32_t Words = Bits / 32u;
    uint32_t Rest = Bits % 32u;
    uint32_t Count;
    uint32_t Index;

    if (A->Count == 0)
        return 0;
    Count = A->Count + Words + 1u;
    if (Count > BOXASM_BIG_LIMBS)
        return -1;
    /* From the top down, so every limb is read before it is written over. */
    for (Index = Count; Index-- > 0;) {
        uint32_t High = Index >= Words && Index - Words < A->Count ? A->Limb[Index - Words] : 0;
        uint32_t Low = Rest != 0 && Index >= Words + 1u && Index - Words - 1u < A->Count
                       ? A->Limb[Index - Words - 1u] : 0;

        A->Limb[Index] = Rest != 0 ? (High << Rest) | (Low >> (32u - Rest)) : High;
    }
    A->Count = Count;
    BoxAsmBigTrim(A);
    return 0;
}

static void BoxAsmBigShiftRightOne(BoxAsmBig *A)
{
    uint32_t Index;

    for (Index = 0; Index < A->Count; Index++) {
        uint32_t Next = Index + 1u < A->Count ? A->Limb[Index + 1u] : 0;

        A->Limb[Index] = (A->Limb[Index] >> 1) | (Next << 31);
    }
    BoxAsmBigTrim(A);
}

static uint32_t BoxAsmBigBitLength(const BoxAsmBig *A)
{
    uint32_t Top;
    uint32_t Bits = 0;

    if (A->Count == 0)
        return 0;
    for (Top = A->Limb[A->Count - 1u]; Top != 0; Top >>= 1)
        Bits++;
    return (A->Count - 1u) * 32u + Bits;
}

static int BoxAsmBigCompare(const BoxAsmBig *A, const BoxAsmBig *B)
{
    uint32_t Index;

    if (A->Count != B->Count)
        return A->Count < B->Count ? -1 : 1;
    for (Index = A->Count; Index-- > 0;)
        if (A->Limb[Index] != B->Limb[Index])
            return A->Limb[Index] < B->Limb[Index] ? -1 : 1;
    return 0;
}

/* A - B, for A no smaller than B. */
static void BoxAsmBigSubtract(BoxAsmBig *A, const BoxAsmBig *B)
{
    uint64_t Borrow = 0;
    uint32_t Index;

    for (Index = 0; Index < A->Count; Index++) {
        uint64_t Take = (uint64_t)(Index < B->Count ? B->Limb[Index] : 0) + Borrow;
        uint64_t Have = A->Limb[Index];

        A->Limb[Index] = (uint32_t)(Have - Take);
        Borrow = Have < Take;
    }
    BoxAsmBigTrim(A);
}

/* Num / Den, its remainder left in Num, for a quotient of at most 64 bits. */
static uint64_t BoxAsmBigDivide(BoxAsmBig *Num, const BoxAsmBig *Den)
{
    BoxAsmBig Step = *Den;
    uint32_t NumBits = BoxAsmBigBitLength(Num);
    uint32_t DenBits = BoxAsmBigBitLength(Den);
    uint64_t Quotient = 0;
    uint32_t Shift;

    if (NumBits < DenBits)
        return 0;
    Shift = NumBits - DenBits;
    (void)BoxAsmBigShiftLeft(&Step, Shift);   /* no wider than Num */
    for (;;) {
        Quotient <<= 1;
        if (BoxAsmBigCompare(Num, &Step) >= 0) {
            BoxAsmBigSubtract(Num, &Step);
            Quotient |= 1u;
        }
        if (Shift-- == 0)
            return Quotient;
        BoxAsmBigShiftRightOne(&Step);
    }
}

/* The value the text writes, as Digits (their integer) times 10^Exponent;
 * a digit past the ones kept leaves *Sticky set when it is not zero. */
static void BoxAsmFloatDigits(const char *Text, size_t Length, BoxAsmBig *Digits,
                              int32_t *Exponent, int *Sticky)
{
    uint32_t Kept = 0;
    int32_t Scale = 0;
    int32_t Written = 0;
    int Point = 0;
    int Minus = 0;
    size_t At;

    BoxAsmBigSet(Digits, 0);
    *Sticky = 0;
    for (At = 0; At < Length; At++) {
        char Ch = Text[At];

        if (Ch == '_')
            continue;
        if (Ch == '.') {
            Point = 1;
            continue;
        }
        if (Ch < '0' || Ch > '9')
            break;
        if (Kept == 0 && Ch == '0') {
            Scale -= Point;
        } else if (Kept < BOXASM_FLOAT_DIGITS) {
            (void)BoxAsmBigMultiplyAdd(Digits, 10u, (uint32_t)(Ch - '0'));
            Kept++;
            Scale -= Point;
        } else {
            *Sticky |= Ch != '0';
            Scale += !Point;
        }
    }
    if (At < Length) {
        At++;
        if (At < Length && (Text[At] == '+' || Text[At] == '-'))
            Minus = Text[At++] == '-';
        for (; At < Length; At++)
            if (Text[At] != '_' && Written < 100000)
                Written = Written * 10 + (Text[At] - '0');
    }
    *Exponent = Scale + (Minus ? -Written : Written);
}

int BoxAsmFloatBits(const char *Text, size_t Length, int Negative, uint8_t Width,
                    uint64_t *Bits, const char **Why)
{
    const BoxAsmFloatFormat *Format = Width == 4 ? &BoxAsmBinary32 : &BoxAsmBinary64;
    const int32_t Precision = Format->Precision;
    uint64_t Sign = Negative ? (uint64_t)1 << (Width * 8u - 1u) : 0;
    BoxAsmBig Num;
    BoxAsmBig Den;
    int32_t Exponent;
    int32_t Scale;
    int32_t Lowest;
    int32_t Shift;
    uint64_t Quotient;
    uint64_t Mantissa;
    int Sticky;
    int Half;
    int Below;

    BoxAsmFloatDigits(Text, Length, &Num, &Exponent, &Sticky);
    if (Num.Count == 0) {
        *Bits = Sign;
        return 0;
    }
    /* Num stands between 10^(digits - 1) and 10^digits. */
    {
        int32_t Magnitude = (int32_t)((BoxAsmBigBitLength(&Num) * 30103u) / 100000u) + Exponent;

        if (Magnitude > BOXASM_FLOAT_LARGEST) {
            *Why = Width == 4 ? "does not fit a 32-bit float (dd)" : "does not fit a 64-bit float (dq)";
            return -1;
        }
        if (Magnitude < BOXASM_FLOAT_SMALLEST) {
            *Why = Width == 4 ? "is too small for a 32-bit float (dd): it would be 0"
                              : "is too small for a 64-bit float (dq): it would be 0";
            return -1;
        }
    }
    /* Value = Num / Den, exactly; then a quotient of Precision + 3 or + 4 bits
     * of it, Num scaled by 2^Scale to get it, and the remainder as sticky. */
    BoxAsmBigSet(&Den, 1u);
    if ((Exponent >= 0 ? BoxAsmBigTimesTen(&Num, (uint32_t)Exponent)
                       : BoxAsmBigTimesTen(&Den, (uint32_t)-Exponent)) != 0) {
        *Why = "has more digits than BoxASM can weigh";
        return -1;
    }
    Scale = Precision + 3 + (int32_t)BoxAsmBigBitLength(&Den) - (int32_t)BoxAsmBigBitLength(&Num);
    if ((Scale >= 0 ? BoxAsmBigShiftLeft(&Num, (uint32_t)Scale)
                    : BoxAsmBigShiftLeft(&Den, (uint32_t)-Scale)) != 0) {
        *Why = "has more digits than BoxASM can weigh";
        return -1;
    }
    Quotient = BoxAsmBigDivide(&Num, &Den);
    Sticky |= Num.Count != 0;

    /* Value = Quotient * 2^-Scale. Its lowest kept bit is the one Precision
     * bits below its top, or the subnormals' when it is below that. */
    {
        uint32_t Length2 = 0;
        int32_t Top;

        for (Mantissa = Quotient; Mantissa != 0; Mantissa >>= 1)
            Length2++;
        Top = (int32_t)Length2 - 1 - Scale;
        Lowest = Top - (Precision - 1);
        if (Lowest < Format->MinExponent - (Precision - 1))
            Lowest = Format->MinExponent - (Precision - 1);
    }
    Shift = Lowest + Scale;
    Mantissa = Shift >= 64 ? 0 : Quotient >> Shift;
    Half = Shift - 1 >= 64 ? 0 : (int)((Quotient >> (Shift - 1)) & 1u);
    Below = Sticky || (Shift - 1 >= 64 ? Quotient != 0
                                       : (Quotient & (((uint64_t)1 << (Shift - 1)) - 1u)) != 0);
    if (Half && (Below || (Mantissa & 1u)))
        Mantissa++;
    if (Mantissa == 0) {
        *Why = Width == 4 ? "is too small for a 32-bit float (dd): it would be 0"
                          : "is too small for a 64-bit float (dq): it would be 0";
        return -1;
    }
    if (Mantissa >> Precision) {
        Mantissa >>= 1;
        Lowest++;
    }
    if (Mantissa >> (Precision - 1)) {
        int32_t Field = Lowest + (Precision - 1) + Format->Bias;

        if (Field > 2 * Format->Bias) {
            *Why = Width == 4 ? "does not fit a 32-bit float (dd)" : "does not fit a 64-bit float (dq)";
            return -1;
        }
        *Bits = Sign | (uint64_t)Field << (Precision - 1) |
                (Mantissa & (((uint64_t)1 << (Precision - 1)) - 1u));
    } else {
        *Bits = Sign | Mantissa;
    }
    return 0;
}

static int BoxAsmWordIs(const char *Text, size_t Length, const char *Word)
{
    return strlen(Word) == Length && memcmp(Text, Word, Length) == 0;
}

int BoxAsmFloatSpecial(const char *Text, size_t Length, int Negative, uint8_t Width,
                       uint64_t *Bits)
{
    const BoxAsmFloatFormat *Format = Width == 4 ? &BoxAsmBinary32 : &BoxAsmBinary64;
    uint64_t Sign = Negative ? (uint64_t)1 << (Width * 8u - 1u) : 0;
    uint64_t Ones = (uint64_t)(2 * Format->Bias + 1) << (Format->Precision - 1u);

    if (BoxAsmWordIs(Text, Length, "__Infinity__"))
        *Bits = Sign | Ones;
    else if (BoxAsmWordIs(Text, Length, "__QNaN__") || BoxAsmWordIs(Text, Length, "__NaN__"))
        *Bits = Sign | Ones | (uint64_t)1 << (Format->Precision - 2u);
    else if (BoxAsmWordIs(Text, Length, "__SNaN__"))
        *Bits = Sign | Ones | 1u;
    else
        return 0;
    return 1;
}
