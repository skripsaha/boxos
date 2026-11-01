/* format.c — the conversions of C's printf, for every sink BoxLib writes to.
 *
 * A floating-point value is written from its own digits: the binary value
 * M x 2^E has a finite decimal expansion, and its integer part (base 10^9
 * chunks) and fraction (a big number times ten, digit by digit) give every
 * one of them exactly. Rounding to the precision asked looks at the digit
 * after the last one kept and at whether anything nonzero follows, so a tie
 * is a real tie and goes to even. The digits are read twice — once to learn
 * the rounding, the carry and the length the width needs, once to write —
 * so nothing is buffered but the integer part's chunks, and no memory is
 * allocated: printf runs where malloc may not. */

#include "box/core/format.h"

#include "box/string.h"

#define BOX_FORMAT_INT_MAX 0x7FFFFFFF

/* The types and the width C names in headers BoxLib does not take from the
 * compiler: %j's, %ls's characters, and the long double's significand. */
typedef __INTMAX_TYPE__ BoxFormatIntMax;
typedef __UINTMAX_TYPE__ BoxFormatUintMax;
typedef __WCHAR_TYPE__ BoxFormatWide;

typedef struct {
    uint8_t Left;        /* '-' */
    uint8_t Plus;        /* '+' */
    uint8_t Space;       /* ' ' */
    uint8_t Alternate;   /* '#' */
    uint8_t Zero;        /* '0' */
    int     Width;
    int     Precision;   /* -1: not given */
    char    Length;      /* 0, 'H' (hh), 'h', 'l', 'q' (ll), 'j', 'z', 't', 'L' */
    char    Conversion;
} BoxFormatSpec;

typedef struct {
    BoxFormatSink *Sink;
    size_t         Written;
    int            Failed;
} BoxFormatOut;

/* ---- Output -------------------------------------------------------------- */

static void BoxFormatEmit(BoxFormatOut *Out, const char *Bytes, size_t Count)
{
    if (Count == 0)
        return;
    Out->Sink->Put(Out->Sink, Bytes, Count);
    Out->Written += Count;
}

static void BoxFormatRepeat(BoxFormatOut *Out, char Ch, size_t Count)
{
    char Run[32];

    memset(Run, Ch, sizeof(Run));
    while (Count != 0) {
        size_t Step = Count < sizeof(Run) ? Count : sizeof(Run);

        BoxFormatEmit(Out, Run, Step);
        Count -= Step;
    }
}

/* What stands before a converted value of Body bytes: the spaces or zeros of
 * the width, its sign and its prefix. Returns the spaces owed after it. */
static size_t BoxFormatOpen(BoxFormatOut *Out, const BoxFormatSpec *Spec, const char *Sign,
                            const char *Prefix, size_t Body, int ZeroPads)
{
    size_t SignLength = Sign ? strlen(Sign) : 0;
    size_t PrefixLength = Prefix ? strlen(Prefix) : 0;
    size_t Total = SignLength + PrefixLength + Body;
    size_t Pad = (size_t)Spec->Width > Total ? (size_t)Spec->Width - Total : 0;
    int Zeros = ZeroPads && Spec->Zero && !Spec->Left;

    if (!Spec->Left && !Zeros)
        BoxFormatRepeat(Out, ' ', Pad);
    BoxFormatEmit(Out, Sign, SignLength);
    BoxFormatEmit(Out, Prefix, PrefixLength);
    if (Zeros)
        BoxFormatRepeat(Out, '0', Pad);
    return Spec->Left ? Pad : 0;
}

static const char *BoxFormatSign(const BoxFormatSpec *Spec, int Negative)
{
    if (Negative)
        return "-";
    if (Spec->Plus)
        return "+";
    return Spec->Space ? " " : NULL;
}

/* ---- Integers, characters, strings ---------------------------------------- */

static void BoxFormatInteger(BoxFormatOut *Out, const BoxFormatSpec *Spec, int Signed,
                             int Negative, uint64_t Magnitude, unsigned Base,
                             const char *Prefix)
{
    const char *Table = Spec->Conversion == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    char Digits[24];
    size_t Count = 0;
    size_t Zeros;
    size_t After;

    /* A precision of 0 and a value of 0 have no digits at all. */
    if (Spec->Precision != 0 || Magnitude != 0) {
        do {
            Digits[Count++] = Table[Magnitude % Base];
            Magnitude /= Base;
        } while (Magnitude != 0);
    }
    Zeros = Spec->Precision > (int)Count ? (size_t)Spec->Precision - Count : 0;
    /* '#' on octal raises the precision just enough for a leading zero. */
    if (Spec->Alternate && Base == 8 && Zeros == 0 && (Count == 0 || Digits[Count - 1] != '0'))
        Zeros = 1;
    After = BoxFormatOpen(Out, Spec, Signed ? BoxFormatSign(Spec, Negative) : NULL, Prefix,
                          Zeros + Count, Spec->Precision < 0);
    BoxFormatRepeat(Out, '0', Zeros);
    while (Count != 0)
        BoxFormatEmit(Out, &Digits[--Count], 1);
    BoxFormatRepeat(Out, ' ', After);
}

/* The UTF-8 bytes of one wide character; 0 when it has none. */
static size_t BoxFormatUtf8(uint32_t Code, char *Bytes)
{
    if (Code < 0x80) {
        Bytes[0] = (char)Code;
        return 1;
    }
    if (Code < 0x800) {
        Bytes[0] = (char)(0xC0 | (Code >> 6));
        Bytes[1] = (char)(0x80 | (Code & 0x3F));
        return 2;
    }
    if (Code >= 0xD800 && Code <= 0xDFFF)
        return 0;
    if (Code < 0x10000) {
        Bytes[0] = (char)(0xE0 | (Code >> 12));
        Bytes[1] = (char)(0x80 | ((Code >> 6) & 0x3F));
        Bytes[2] = (char)(0x80 | (Code & 0x3F));
        return 3;
    }
    if (Code < 0x110000) {
        Bytes[0] = (char)(0xF0 | (Code >> 18));
        Bytes[1] = (char)(0x80 | ((Code >> 12) & 0x3F));
        Bytes[2] = (char)(0x80 | ((Code >> 6) & 0x3F));
        Bytes[3] = (char)(0x80 | (Code & 0x3F));
        return 4;
    }
    return 0;
}

static void BoxFormatText(BoxFormatOut *Out, const BoxFormatSpec *Spec, const char *Text,
                          size_t Length)
{
    size_t After = BoxFormatOpen(Out, Spec, NULL, NULL, Length, 0);

    BoxFormatEmit(Out, Text, Length);
    BoxFormatRepeat(Out, ' ', After);
}

/* %s, or %ls as UTF-8. A precision bounds how far the argument is read, so
 * it need not end in a NUL, and for %ls it counts bytes, never splitting a
 * character. */
static void BoxFormatString(BoxFormatOut *Out, const BoxFormatSpec *Spec, const void *Argument)
{
    const BoxFormatWide *Wide = (const BoxFormatWide *)Argument;
    size_t Bytes = 0;
    size_t Index;
    size_t After;

    if (Spec->Length != 'l') {
        const char *Text = Argument ? (const char *)Argument : "(null)";
        size_t Length = 0;

        while ((Spec->Precision < 0 || Length < (size_t)Spec->Precision) && Text[Length] != '\0')
            Length++;
        BoxFormatText(Out, Spec, Text, Length);
        return;
    }
    if (!Wide) {
        BoxFormatText(Out, Spec, "(null)",
                      Spec->Precision >= 0 && Spec->Precision < 6 ? (size_t)Spec->Precision : 6);
        return;
    }
    for (Index = 0; Wide[Index] != 0; Index++) {
        char Encoded[4];
        size_t Length = BoxFormatUtf8((uint32_t)Wide[Index], Encoded);

        if (Length == 0) {
            Out->Failed = 1;
            return;
        }
        if (Spec->Precision >= 0 && Bytes + Length > (size_t)Spec->Precision)
            break;
        Bytes += Length;
    }
    After = BoxFormatOpen(Out, Spec, NULL, NULL, Bytes, 0);
    for (Index = 0; Bytes != 0; Index++) {
        char Encoded[4];
        size_t Length = BoxFormatUtf8((uint32_t)Wide[Index], Encoded);

        BoxFormatEmit(Out, Encoded, Length);
        Bytes -= Length;
    }
    BoxFormatRepeat(Out, ' ', After);
}

/* ---- Floating point: the value ------------------------------------------- */

typedef enum {
    BOX_FLOAT_ZERO = 0,
    BOX_FLOAT_FINITE,
    BOX_FLOAT_INFINITE,
    BOX_FLOAT_NAN
} BoxFloatKind;

/* A value as Mantissa x 2^Exponent, its significand Bits wide (53 or 64). */
typedef struct {
    uint8_t  Negative;
    uint8_t  Kind;
    uint8_t  Bits;
    uint64_t Mantissa;
    int32_t  Exponent;
} BoxFormatFloat;

static void BoxFormatDecodeDouble(double Value, BoxFormatFloat *Out)
{
    uint64_t Raw;
    uint32_t Field;
    uint64_t Fraction;

    memcpy(&Raw, &Value, sizeof(Raw));
    Field = (uint32_t)(Raw >> 52) & 0x7FFu;
    Fraction = Raw & (((uint64_t)1 << 52) - 1u);
    Out->Negative = (uint8_t)(Raw >> 63);
    Out->Bits = 53;
    if (Field == 0x7FFu) {
        Out->Kind = Fraction != 0 ? BOX_FLOAT_NAN : BOX_FLOAT_INFINITE;
    } else if (Field == 0) {
        Out->Kind = Fraction != 0 ? BOX_FLOAT_FINITE : BOX_FLOAT_ZERO;
        Out->Mantissa = Fraction;
        Out->Exponent = -1074;
    } else {
        Out->Kind = BOX_FLOAT_FINITE;
        Out->Mantissa = Fraction | ((uint64_t)1 << 52);
        Out->Exponent = (int32_t)Field - 1075;
    }
}

static void BoxFormatDecodeLongDouble(long double Value, BoxFormatFloat *Out)
{
#if __LDBL_MANT_DIG__ == 64
    /* x87 extended: 64 significand bits, the integer bit among them, then 15
     * bits of exponent and the sign. */
    unsigned char Raw[sizeof(long double)];
    uint64_t Mantissa;
    uint16_t Top;
    uint32_t Field;

    memcpy(Raw, &Value, sizeof(Raw));
    memcpy(&Mantissa, Raw, sizeof(Mantissa));
    memcpy(&Top, Raw + 8, sizeof(Top));
    Field = Top & 0x7FFFu;
    Out->Negative = (uint8_t)(Top >> 15);
    Out->Bits = 64;
    Out->Mantissa = Mantissa;
    if (Field == 0x7FFFu) {
        Out->Kind = (Mantissa << 1) != 0 ? BOX_FLOAT_NAN : BOX_FLOAT_INFINITE;
    } else if (Field == 0) {
        Out->Kind = Mantissa != 0 ? BOX_FLOAT_FINITE : BOX_FLOAT_ZERO;
        Out->Exponent = -16445;
    } else if ((Mantissa >> 63) == 0) {
        Out->Kind = BOX_FLOAT_NAN;           /* an unnormal: the processor's invalid operand */
    } else {
        Out->Kind = BOX_FLOAT_FINITE;
        Out->Exponent = (int32_t)Field - 16446;
    }
#elif __LDBL_MANT_DIG__ == 53
    BoxFormatDecodeDouble((double)Value, Out);
#else
#error "long double is neither binary64 nor x87 extended"
#endif
}

/* ---- Floating point: big numbers ----------------------------------------- */

typedef struct {
    uint32_t *Limb;
    uint32_t  Capacity;
    uint32_t  Count;     /* limbs in use, the top one not zero */
} BoxFormatBig;

static void BoxFormatBigTrim(BoxFormatBig *Big)
{
    while (Big->Count != 0 && Big->Limb[Big->Count - 1u] == 0)
        Big->Count--;
}

/* Mantissa x 2^Shift. */
static void BoxFormatBigSet(BoxFormatBig *Big, uint64_t Mantissa, uint32_t Shift)
{
    uint32_t Words = Shift / 32u;
    uint32_t Rest = Shift % 32u;
    uint64_t Low = Mantissa << Rest;
    uint32_t High = Rest == 0 ? 0 : (uint32_t)(Mantissa >> (64u - Rest));
    uint32_t Index;

    for (Index = 0; Index < Words; Index++)
        Big->Limb[Index] = 0;
    Big->Limb[Words] = (uint32_t)Low;
    Big->Limb[Words + 1u] = (uint32_t)(Low >> 32);
    Big->Limb[Words + 2u] = High;
    Big->Count = Words + 3u;
    BoxFormatBigTrim(Big);
}

/* Big / Divisor, the quotient left in Big; returns the remainder. */
static uint32_t BoxFormatBigDivide(BoxFormatBig *Big, uint32_t Divisor)
{
    uint64_t Rest = 0;
    uint32_t Index;

    for (Index = Big->Count; Index-- > 0;) {
        uint64_t Part = (Rest << 32) | Big->Limb[Index];

        Big->Limb[Index] = (uint32_t)(Part / Divisor);
        Rest = Part % Divisor;
    }
    BoxFormatBigTrim(Big);
    return (uint32_t)Rest;
}

/* Fraction x 10, the part reaching bit Bits and above taken off and
 * returned: the next decimal digit of Fraction / 2^Bits. */
static uint32_t BoxFormatBigNextDigit(BoxFormatBig *Fraction, uint32_t Bits)
{
    uint64_t Carry = 0;
    uint32_t Top = Bits / 32u;
    uint32_t Shift = Bits % 32u;
    uint32_t Index;
    uint32_t Digit;

    for (Index = 0; Index < Fraction->Count; Index++) {
        uint64_t Product = (uint64_t)Fraction->Limb[Index] * 10u + Carry;

        Fraction->Limb[Index] = (uint32_t)Product;
        Carry = Product >> 32;
    }
    if (Carry != 0)
        Fraction->Limb[Fraction->Count++] = (uint32_t)Carry;
    while (Fraction->Count <= Top + 1u)
        Fraction->Limb[Fraction->Count++] = 0;
    Digit = (uint32_t)((((uint64_t)Fraction->Limb[Top + 1u] << 32) | Fraction->Limb[Top]) >> Shift);
    Fraction->Limb[Top] &= Shift != 0 ? (1u << Shift) - 1u : 0;
    Fraction->Limb[Top + 1u] = 0;
    Fraction->Count = Top + 2u;
    BoxFormatBigTrim(Fraction);
    return Digit;
}

/* ---- Floating point: the digits ------------------------------------------ */

/* The decimal digits of a finite value, most significant first: those of its
 * integer part, then those of its fraction, then zeros for ever. */
typedef struct {
    const BoxFormatFloat *Value;
    uint32_t     *Chunk;          /* the integer part in base 10^9, least first */
    uint32_t      ChunkCount;
    uint32_t      IntegerDigits;  /* 0 when the integer part is 0 */
    BoxFormatBig  Fraction;       /* numerator over 2^FractionBits */
    uint32_t      FractionBits;
    uint32_t      Read;           /* digits handed out */
} BoxFormatDigits;

static const uint32_t BoxFormatPowers[10] = {
    1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u, 100000000u, 1000000000u
};

/* The integer part, once: its chunks and how many digits it has. */
static void BoxFormatDigitsBegin(BoxFormatDigits *Digits, const BoxFormatFloat *Value,
                                 uint32_t *Limb, uint32_t LimbCapacity, uint32_t *Chunk)
{
    uint64_t Whole;
    uint32_t Top;
    uint32_t Count;

    Digits->Value = Value;
    Digits->Chunk = Chunk;
    Digits->ChunkCount = 0;
    Digits->Fraction.Limb = Limb;
    Digits->Fraction.Capacity = LimbCapacity;
    Digits->Fraction.Count = 0;
    Digits->FractionBits = Value->Exponent < 0 ? (uint32_t)-Value->Exponent : 0;
    if (Value->Kind != BOX_FLOAT_FINITE) {
        Digits->IntegerDigits = 0;
        return;
    }
    if (Value->Exponent >= 0) {
        BoxFormatBig Integer = { Limb, LimbCapacity, 0 };

        BoxFormatBigSet(&Integer, Value->Mantissa, (uint32_t)Value->Exponent);
        while (Integer.Count != 0)
            Chunk[Digits->ChunkCount++] = BoxFormatBigDivide(&Integer, 1000000000u);
    } else {
        Whole = Digits->FractionBits >= 64 ? 0 : Value->Mantissa >> Digits->FractionBits;
        while (Whole != 0) {
            Chunk[Digits->ChunkCount++] = (uint32_t)(Whole % 1000000000u);
            Whole /= 1000000000u;
        }
    }
    if (Digits->ChunkCount == 0) {
        Digits->IntegerDigits = 0;
        return;
    }
    Top = Chunk[Digits->ChunkCount - 1u];
    for (Count = 1; Count < 10 && Top >= BoxFormatPowers[Count]; Count++)
        ;
    Digits->IntegerDigits = (Digits->ChunkCount - 1u) * 9u + Count;
}

/* Back to the first digit, the fraction made again from the mantissa. */
static void BoxFormatDigitsRewind(BoxFormatDigits *Digits)
{
    uint64_t Fraction = 0;

    Digits->Read = 0;
    if (Digits->Value->Kind == BOX_FLOAT_FINITE && Digits->FractionBits != 0)
        Fraction = Digits->FractionBits >= 64
                   ? Digits->Value->Mantissa
                   : Digits->Value->Mantissa & (((uint64_t)1 << Digits->FractionBits) - 1u);
    BoxFormatBigSet(&Digits->Fraction, Fraction, 0);
}

static uint32_t BoxFormatIntegerDigit(const BoxFormatDigits *Digits, uint32_t Index)
{
    uint32_t Place = Digits->IntegerDigits - 1u - Index;

    return (Digits->Chunk[Place / 9u] / BoxFormatPowers[Place % 9u]) % 10u;
}

static uint32_t BoxFormatNextDigit(BoxFormatDigits *Digits)
{
    uint32_t Index = Digits->Read++;

    if (Index < Digits->IntegerDigits)
        return BoxFormatIntegerDigit(Digits, Index);
    if (Digits->Fraction.Count == 0)
        return 0;
    return BoxFormatBigNextDigit(&Digits->Fraction, Digits->FractionBits);
}

/* Whether any digit after the ones read is not zero. */
static int BoxFormatRestNonzero(const BoxFormatDigits *Digits)
{
    uint32_t Place;
    uint32_t Index;

    if (Digits->Fraction.Count != 0)
        return 1;
    if (Digits->Read >= Digits->IntegerDigits)
        return 0;
    Place = Digits->IntegerDigits - Digits->Read;    /* the digits still to come */
    if (Place % 9u != 0 && Digits->Chunk[Place / 9u] % BoxFormatPowers[Place % 9u] != 0)
        return 1;
    for (Index = 0; Index < Place / 9u; Index++)
        if (Digits->Chunk[Index] != 0)
            return 1;
    return 0;
}

/* A run of Count digits as written: from the cursor, after a leading 0 when
 * Lead is set, rounded at its end. */
typedef struct {
    uint8_t  RoundUp;
    int64_t  LastNotNine;   /* the last digit that is not 9, -1 when all are */
    int64_t  LastNotZero;   /* the last digit not 0 once rounded, -1 when none */
} BoxFormatRun;

static uint32_t BoxFormatRunDigit(BoxFormatDigits *Digits, int Lead, uint64_t Index)
{
    return Lead && Index == 0 ? 0 : BoxFormatNextDigit(Digits);
}

static void BoxFormatMeasure(BoxFormatDigits *Digits, int Lead, uint64_t Count, BoxFormatRun *Run)
{
    int64_t LastNotZero = -1;
    uint32_t Last = 0;
    uint32_t Next;
    uint64_t Index;

    Run->LastNotNine = -1;
    for (Index = 0; Index < Count; Index++) {
        Last = BoxFormatRunDigit(Digits, Lead, Index);
        if (Last != 9)
            Run->LastNotNine = (int64_t)Index;
        if (Last != 0)
            LastNotZero = (int64_t)Index;
    }
    Next = BoxFormatNextDigit(Digits);
    Run->RoundUp = Next > 5 || (Next == 5 && (BoxFormatRestNonzero(Digits) || (Last & 1u)));
    if (Run->RoundUp)
        Run->LastNotZero = Run->LastNotNine >= 0 ? Run->LastNotNine : 0;
    else
        Run->LastNotZero = LastNotZero;
}

/* Writes the run's digits From up to To, rounded as measured, a '.' before
 * digit Point (none when Point is To or past it). */
static void BoxFormatWriteRun(BoxFormatOut *Out, BoxFormatDigits *Digits, int Lead,
                              const BoxFormatRun *Run, uint64_t To, uint64_t Point)
{
    char Batch[64];
    size_t Used = 0;
    uint64_t Index;

    for (Index = 0; Index < To; Index++) {
        uint32_t Digit = BoxFormatRunDigit(Digits, Lead, Index);

        if (Run->RoundUp) {
            if ((int64_t)Index == Run->LastNotNine)
                Digit++;
            else if ((int64_t)Index > Run->LastNotNine)
                Digit = 0;
        }
        if (Index == Point)
            Batch[Used++] = '.';
        Batch[Used++] = (char)('0' + Digit);
        if (Used >= sizeof(Batch) - 2u) {
            BoxFormatEmit(Out, Batch, Used);
            Used = 0;
        }
    }
    BoxFormatEmit(Out, Batch, Used);
}

/* %f: Precision digits after the point; Trim drops the fraction's trailing
 * zeros (%g without '#'). */
static void BoxFormatFixed(BoxFormatOut *Out, const BoxFormatSpec *Spec, BoxFormatDigits *Digits,
                           const char *Sign, uint64_t Precision, int Trim)
{
    int Lead = Digits->IntegerDigits == 0;
    uint64_t Whole = Lead ? 1u : Digits->IntegerDigits;
    uint64_t Fraction = Precision;
    BoxFormatRun Run;
    int Grows;
    int Point;
    size_t After;

    BoxFormatDigitsRewind(Digits);
    BoxFormatMeasure(Digits, Lead, Whole + Precision, &Run);
    Grows = Run.RoundUp && Run.LastNotNine < 0;       /* 9.99 became 10.0 */
    if (Trim)
        Fraction = Grows || Run.LastNotZero < (int64_t)Whole ? 0 : (uint64_t)Run.LastNotZero - Whole + 1u;
    Point = Fraction != 0 || Spec->Alternate;
    After = BoxFormatOpen(Out, Spec, Sign, NULL, (size_t)(Grows + Whole + Point + Fraction), 1);
    if (Grows)
        BoxFormatEmit(Out, "1", 1);
    BoxFormatDigitsRewind(Digits);
    BoxFormatWriteRun(Out, Digits, Lead, &Run, Whole + Fraction, Whole);
    if (Point && Fraction == 0)
        BoxFormatEmit(Out, ".", 1);
    BoxFormatRepeat(Out, ' ', After);
}

/* How many digits come before the first that is not zero, and so the
 * exponent of the value in %e. */
static uint64_t BoxFormatLeadingZeros(BoxFormatDigits *Digits, int64_t *Exponent)
{
    uint64_t Zeros = 0;

    BoxFormatDigitsRewind(Digits);
    if (Digits->Value->Kind == BOX_FLOAT_ZERO) {
        *Exponent = 0;
        return 0;
    }
    if (Digits->IntegerDigits != 0) {
        *Exponent = (int64_t)Digits->IntegerDigits - 1;
        return 0;
    }
    while (BoxFormatNextDigit(Digits) == 0)
        Zeros++;
    *Exponent = -(int64_t)Zeros - 1;
    return Zeros;
}

static void BoxFormatSkip(BoxFormatDigits *Digits, uint64_t Count)
{
    BoxFormatDigitsRewind(Digits);
    while (Count-- != 0)
        (void)BoxFormatNextDigit(Digits);
}

/* The exponent %e writes for Precision digits after the point, its
 * rounding counted: 9.99 at precision 1 is 1.0e+01. */
static int64_t BoxFormatScientificExponent(BoxFormatDigits *Digits, uint64_t Precision)
{
    int64_t Exponent;
    uint64_t Zeros = BoxFormatLeadingZeros(Digits, &Exponent);
    int Lead = Digits->Value->Kind == BOX_FLOAT_ZERO;
    BoxFormatRun Run;

    BoxFormatSkip(Digits, Zeros);
    BoxFormatMeasure(Digits, Lead, Precision + 1u, &Run);
    return Run.RoundUp && Run.LastNotNine < 0 ? Exponent + 1 : Exponent;
}

static void BoxFormatScientific(BoxFormatOut *Out, const BoxFormatSpec *Spec,
                                BoxFormatDigits *Digits, const char *Sign, uint64_t Precision,
                                int Trim, int Upper)
{
    int64_t Exponent;
    uint64_t Zeros = BoxFormatLeadingZeros(Digits, &Exponent);
    int Lead = Digits->Value->Kind == BOX_FLOAT_ZERO;
    uint64_t Fraction = Precision;
    uint64_t Magnitude;
    char Tail[16];
    size_t TailLength = 0;
    char Reversed[8];
    size_t Count = 0;
    BoxFormatRun Run;
    int Carried;
    int Point;
    size_t After;

    BoxFormatSkip(Digits, Zeros);
    BoxFormatMeasure(Digits, Lead, Precision + 1u, &Run);
    Carried = Run.RoundUp && Run.LastNotNine < 0;     /* 9.99 became 1.00 x 10 */
    if (Carried)
        Exponent++;
    if (Trim)
        Fraction = Carried || Run.LastNotZero < 1 ? 0 : (uint64_t)Run.LastNotZero;
    Point = Fraction != 0 || Spec->Alternate;
    Tail[TailLength++] = Upper ? 'E' : 'e';
    Tail[TailLength++] = Exponent < 0 ? '-' : '+';
    Magnitude = Exponent < 0 ? (uint64_t)-Exponent : (uint64_t)Exponent;
    do {
        Reversed[Count++] = (char)('0' + Magnitude % 10u);
        Magnitude /= 10u;
    } while (Magnitude != 0);
    if (Count == 1)
        Reversed[Count++] = '0';
    while (Count != 0)
        Tail[TailLength++] = Reversed[--Count];
    After = BoxFormatOpen(Out, Spec, Sign, NULL, (size_t)(1u + Point + Fraction + TailLength), 1);
    if (Carried) {
        BoxFormatEmit(Out, "1", 1);
        if (Point)
            BoxFormatEmit(Out, ".", 1);
        BoxFormatRepeat(Out, '0', (size_t)Fraction);
    } else {
        BoxFormatSkip(Digits, Zeros);
        BoxFormatWriteRun(Out, Digits, Lead, &Run, 1u + Fraction, 1u);
        if (Point && Fraction == 0)
            BoxFormatEmit(Out, ".", 1);
    }
    BoxFormatEmit(Out, Tail, TailLength);
    BoxFormatRepeat(Out, ' ', After);
}

/* %a: 0x1.hhhp+d, the leading digit 1 for every value but zero; without a
 * precision, as many hex digits as the value needs, and no more. */
static void BoxFormatHex(BoxFormatOut *Out, const BoxFormatSpec *Spec, const BoxFormatFloat *Value,
                         const char *Sign, int Upper)
{
    const char *Table = Upper ? "0123456789ABCDEF" : "0123456789abcdef";
    uint64_t Fraction = 0;
    uint32_t Digits = 0;
    uint32_t Lead = 0;
    int64_t Exponent = 0;
    uint64_t Precision;
    uint64_t Magnitude;
    char Tail[16];
    size_t TailLength = 0;
    char Reversed[8];
    size_t Count = 0;
    char Shown[16];
    uint32_t Index;
    int Point;
    size_t After;

    if (Value->Kind == BOX_FLOAT_FINITE) {
        uint32_t Top = 63;

        while ((Value->Mantissa >> Top) == 0)
            Top--;
        Lead = 1;
        Exponent = (int64_t)Value->Exponent + Top;
        Digits = (Top + 3u) / 4u;
        Fraction = Top == 0 ? 0 : (Value->Mantissa & (((uint64_t)1 << Top) - 1u)) << (Digits * 4u - Top);
    }
    Precision = Spec->Precision >= 0 ? (uint64_t)Spec->Precision : Digits;
    if (Spec->Precision < 0) {
        while (Digits != 0 && (Fraction & 0xFu) == 0) {   /* exact, and no longer */
            Fraction >>= 4;
            Digits--;
        }
        Precision = Digits;
    } else if (Precision < Digits) {
        uint32_t Dropped = (Digits - (uint32_t)Precision) * 4u;
        uint64_t Rest = Fraction & (Dropped == 64 ? ~(uint64_t)0 : (((uint64_t)1 << Dropped) - 1u));
        uint64_t Half = (uint64_t)1 << (Dropped - 1u);
        uint64_t Kept = Dropped == 64 ? 0 : Fraction >> Dropped;
        uint64_t Parity = Precision == 0 ? Lead : Kept;

        if (Rest > Half || (Rest == Half && (Parity & 1u))) {
            Kept++;
            if (Precision < 16 && Kept == (uint64_t)1 << (Precision * 4u)) {
                Kept = 0;
                Lead++;                               /* 0x1.ffp+0 became 0x2.0p+0 */
            }
        }
        Fraction = Kept;
        Digits = (uint32_t)Precision;
    }
    for (Index = 0; Index < Digits; Index++)
        Shown[Index] = Table[(Fraction >> ((Digits - 1u - Index) * 4u)) & 0xFu];
    Point = Precision != 0 || Spec->Alternate;
    Tail[TailLength++] = Upper ? 'P' : 'p';
    Tail[TailLength++] = Exponent < 0 ? '-' : '+';
    Magnitude = Exponent < 0 ? (uint64_t)-Exponent : (uint64_t)Exponent;
    do {
        Reversed[Count++] = (char)('0' + Magnitude % 10u);
        Magnitude /= 10u;
    } while (Magnitude != 0);
    while (Count != 0)
        Tail[TailLength++] = Reversed[--Count];
    After = BoxFormatOpen(Out, Spec, Sign, Upper ? "0X" : "0x",
                          (size_t)(1u + Point + Precision + TailLength), 1);
    BoxFormatEmit(Out, &Table[Lead], 1);
    if (Point)
        BoxFormatEmit(Out, ".", 1);
    BoxFormatEmit(Out, Shown, Digits);
    BoxFormatRepeat(Out, '0', (size_t)(Precision - Digits));
    BoxFormatEmit(Out, Tail, TailLength);
    BoxFormatRepeat(Out, ' ', After);
}

static void BoxFormatFloating(BoxFormatOut *Out, const BoxFormatSpec *Spec,
                              const BoxFormatFloat *Value, uint32_t *Limb, uint32_t LimbCapacity,
                              uint32_t *Chunk)
{
    char Conversion = Spec->Conversion;
    int Upper = Conversion == 'F' || Conversion == 'E' || Conversion == 'G' || Conversion == 'A';
    const char *Sign = BoxFormatSign(Spec, Value->Negative);
    BoxFormatDigits Digits;
    uint64_t Precision;
    int64_t Exponent;

    if (Value->Kind == BOX_FLOAT_INFINITE || Value->Kind == BOX_FLOAT_NAN) {
        const char *Text = Value->Kind == BOX_FLOAT_NAN ? (Upper ? "NAN" : "nan")
                                                        : (Upper ? "INF" : "inf");
        size_t After = BoxFormatOpen(Out, Spec, Sign, NULL, 3, 0);

        BoxFormatEmit(Out, Text, 3);
        BoxFormatRepeat(Out, ' ', After);
        return;
    }
    if (Conversion == 'a' || Conversion == 'A') {
        BoxFormatHex(Out, Spec, Value, Sign, Upper);
        return;
    }
    BoxFormatDigitsBegin(&Digits, Value, Limb, LimbCapacity, Chunk);
    Precision = Spec->Precision >= 0 ? (uint64_t)Spec->Precision : 6u;
    if (Conversion == 'f' || Conversion == 'F') {
        BoxFormatFixed(Out, Spec, &Digits, Sign, Precision, 0);
        return;
    }
    if (Conversion == 'e' || Conversion == 'E') {
        BoxFormatScientific(Out, Spec, &Digits, Sign, Precision, 0, Upper);
        return;
    }
    /* %g: %e's exponent X at P significant digits picks the form — %f with
     * P - 1 - X digits when P > X >= -4, %e with P - 1 otherwise. */
    if (Precision == 0)
        Precision = 1;
    Exponent = BoxFormatScientificExponent(&Digits, Precision - 1u);
    if ((int64_t)Precision > Exponent && Exponent >= -4)
        BoxFormatFixed(Out, Spec, &Digits, Sign, (uint64_t)((int64_t)Precision - 1 - Exponent),
                       !Spec->Alternate);
    else
        BoxFormatScientific(Out, Spec, &Digits, Sign, Precision - 1u, !Spec->Alternate, Upper);
}

/* The room a value's digits need: a double's integer part is under 2^1024
 * (309 digits) and its fraction under 2^1074; an x87 long double's are under
 * 2^16384 (4933 digits) and 2^16445. Each width on its own frame, so a
 * double never pays for the long double's. */
#define BOX_FORMAT_DOUBLE_LIMBS 40u
#define BOX_FORMAT_DOUBLE_CHUNKS 36u
#define BOX_FORMAT_LONG_LIMBS 520u
#define BOX_FORMAT_LONG_CHUNKS 552u

__attribute__((noinline))
static void BoxFormatDouble(BoxFormatOut *Out, const BoxFormatSpec *Spec, double Value)
{
    uint32_t Limb[BOX_FORMAT_DOUBLE_LIMBS];
    uint32_t Chunk[BOX_FORMAT_DOUBLE_CHUNKS];
    BoxFormatFloat Decoded;

    BoxFormatDecodeDouble(Value, &Decoded);
    BoxFormatFloating(Out, Spec, &Decoded, Limb, BOX_FORMAT_DOUBLE_LIMBS, Chunk);
}

__attribute__((noinline))
static void BoxFormatLongDouble(BoxFormatOut *Out, const BoxFormatSpec *Spec, long double Value)
{
    uint32_t Limb[BOX_FORMAT_LONG_LIMBS];
    uint32_t Chunk[BOX_FORMAT_LONG_CHUNKS];
    BoxFormatFloat Decoded;

    BoxFormatDecodeLongDouble(Value, &Decoded);
    BoxFormatFloating(Out, Spec, &Decoded, Limb, BOX_FORMAT_LONG_LIMBS, Chunk);
}

/* ---- The walk ------------------------------------------------------------ */

/* A run of decimal digits, saturating so no width or precision can overflow. */
static int BoxFormatNumber(const char **Format)
{
    int Value = 0;

    while (**Format >= '0' && **Format <= '9') {
        int Digit = *(*Format)++ - '0';

        Value = Value > (BOX_FORMAT_INT_MAX - Digit) / 10 ? BOX_FORMAT_INT_MAX : Value * 10 + Digit;
    }
    return Value;
}

static const char *BoxFormatParse(const char *Format, BoxFormatSpec *Spec, va_list *Args)
{
    memset(Spec, 0, sizeof(*Spec));
    Spec->Precision = -1;
    for (;; Format++) {
        if (*Format == '-')      Spec->Left = 1;
        else if (*Format == '+') Spec->Plus = 1;
        else if (*Format == ' ') Spec->Space = 1;
        else if (*Format == '#') Spec->Alternate = 1;
        else if (*Format == '0') Spec->Zero = 1;
        else break;
    }
    if (*Format == '*') {
        int Width = va_arg(*Args, int);

        Format++;
        if (Width < 0) {                       /* a negative width is '-' and its size */
            Spec->Left = 1;
            Width = Width == -BOX_FORMAT_INT_MAX - 1 ? BOX_FORMAT_INT_MAX : -Width;
        }
        Spec->Width = Width;
    } else {
        Spec->Width = BoxFormatNumber(&Format);
    }
    if (*Format == '.') {
        Format++;
        if (*Format == '*') {
            Format++;
            Spec->Precision = va_arg(*Args, int);
            if (Spec->Precision < 0)           /* a negative precision is none */
                Spec->Precision = -1;
        } else {
            Spec->Precision = BoxFormatNumber(&Format);
        }
    }
    switch (*Format) {
    case 'h':
        Spec->Length = Format[1] == 'h' ? 'H' : 'h';
        Format += Format[1] == 'h' ? 2 : 1;
        break;
    case 'l':
        Spec->Length = Format[1] == 'l' ? 'q' : 'l';
        Format += Format[1] == 'l' ? 2 : 1;
        break;
    case 'j': case 'z': case 't': case 'L':
        Spec->Length = *Format++;
        break;
    default:
        break;
    }
    Spec->Conversion = *Format;
    return Format;
}

static int64_t BoxFormatSigned(const BoxFormatSpec *Spec, va_list *Args)
{
    switch (Spec->Length) {
    case 'H': return (signed char)va_arg(*Args, int);
    case 'h': return (short)va_arg(*Args, int);
    case 'l': return va_arg(*Args, long);
    case 'q': return va_arg(*Args, long long);
    case 'j': return va_arg(*Args, BoxFormatIntMax);
    case 'z': return (int64_t)va_arg(*Args, size_t);
    case 't': return va_arg(*Args, ptrdiff_t);
    default:  return va_arg(*Args, int);
    }
}

static uint64_t BoxFormatUnsigned(const BoxFormatSpec *Spec, va_list *Args)
{
    switch (Spec->Length) {
    case 'H': return (unsigned char)va_arg(*Args, unsigned int);
    case 'h': return (unsigned short)va_arg(*Args, unsigned int);
    case 'l': return va_arg(*Args, unsigned long);
    case 'q': return va_arg(*Args, unsigned long long);
    case 'j': return va_arg(*Args, BoxFormatUintMax);
    case 'z': return va_arg(*Args, size_t);
    case 't': return (uint64_t)va_arg(*Args, ptrdiff_t);
    default:  return va_arg(*Args, unsigned int);
    }
}

static void BoxFormatStore(const BoxFormatSpec *Spec, void *Target, size_t Written)
{
    switch (Spec->Length) {
    case 'H': *(signed char *)Target = (signed char)Written; break;
    case 'h': *(short *)Target = (short)Written; break;
    case 'l': *(long *)Target = (long)Written; break;
    case 'q': *(long long *)Target = (long long)Written; break;
    case 'j': *(BoxFormatIntMax *)Target = (BoxFormatIntMax)Written; break;
    case 'z': *(size_t *)Target = Written; break;
    case 't': *(ptrdiff_t *)Target = (ptrdiff_t)Written; break;
    default:  *(int *)Target = (int)Written; break;
    }
}

/* One conversion, its arguments taken from Args. */
static void BoxFormatConvert(BoxFormatOut *Out, const BoxFormatSpec *Spec, va_list *Args)
{
    switch (Spec->Conversion) {
    case 'd': case 'i': {
        int64_t Value = BoxFormatSigned(Spec, Args);

        BoxFormatInteger(Out, Spec, 1, Value < 0,
                         Value < 0 ? (uint64_t)0 - (uint64_t)Value : (uint64_t)Value, 10, NULL);
        break;
    }
    case 'o': case 'u': case 'x': case 'X': {
        uint64_t Value = BoxFormatUnsigned(Spec, Args);
        unsigned Base = Spec->Conversion == 'o' ? 8 : Spec->Conversion == 'u' ? 10 : 16;
        const char *Prefix = Spec->Alternate && Base == 16 && Value != 0
                             ? (Spec->Conversion == 'X' ? "0X" : "0x") : NULL;

        BoxFormatInteger(Out, Spec, 0, 0, Value, Base, Prefix);
        break;
    }
    case 'c': {
        char Encoded[4];
        size_t Length = 1;

        if (Spec->Length == 'l') {
            Length = BoxFormatUtf8((uint32_t)va_arg(*Args, unsigned int), Encoded);
            if (Length == 0) {
                Out->Failed = 1;
                break;
            }
        } else {
            Encoded[0] = (char)(unsigned char)va_arg(*Args, int);
        }
        BoxFormatText(Out, Spec, Encoded, Length);
        break;
    }
    case 's':
        BoxFormatString(Out, Spec, va_arg(*Args, const void *));
        break;
    case 'p':
        BoxFormatInteger(Out, Spec, 0, 0, (uint64_t)(uintptr_t)va_arg(*Args, void *), 16, "0x");
        break;
    case 'n':
        BoxFormatStore(Spec, va_arg(*Args, void *), Out->Written);
        break;
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
        if (Spec->Length == 'L')
            BoxFormatLongDouble(Out, Spec, va_arg(*Args, long double));
        else
            BoxFormatDouble(Out, Spec, va_arg(*Args, double));
        break;
    default: {
        /* Undefined in C; written back, so the author sees what was written. */
        char Unknown[2] = { '%', Spec->Conversion };

        BoxFormatEmit(Out, Unknown, 2);
        break;
    }
    }
}

int BoxFormat(BoxFormatSink *Sink, const char *Format, va_list Args)
{
    BoxFormatOut Out = { Sink, 0, 0 };
    va_list Walk;

    if (!Sink || !Format)
        return -1;
    va_copy(Walk, Args);
    while (*Format != '\0') {
        const char *Text = Format;
        BoxFormatSpec Spec;

        while (*Format != '\0' && *Format != '%')
            Format++;
        BoxFormatEmit(&Out, Text, (size_t)(Format - Text));
        if (*Format == '\0')
            break;
        Format++;
        if (*Format == '\0') {                 /* a '%' that ends the format is itself */
            BoxFormatEmit(&Out, "%", 1);
            break;
        }
        if (*Format == '%') {
            BoxFormatEmit(&Out, "%", 1);
            Format++;
            continue;
        }
        /* BoxOS: %color and %bgcolor take a Color and change the text's
         * foreground or background from here on. */
        if (Sink->Color && strncmp(Format, "color", 5) == 0) {
            Sink->Color(Sink, 0, va_arg(Walk, uint32_t));
            Format += 5;
            continue;
        }
        if (Sink->Color && strncmp(Format, "bgcolor", 7) == 0) {
            Sink->Color(Sink, 1, va_arg(Walk, uint32_t));
            Format += 7;
            continue;
        }
        Format = BoxFormatParse(Format, &Spec, &Walk);
        if (*Format == '\0') {
            BoxFormatEmit(&Out, "%", 1);
            break;
        }
        BoxFormatConvert(&Out, &Spec, &Walk);
        Format++;
    }
    va_end(Walk);
    if (Out.Failed || Out.Written > (size_t)BOX_FORMAT_INT_MAX)
        return -1;
    return (int)Out.Written;
}

/* ---- Into a buffer -------------------------------------------------------- */

typedef struct {
    BoxFormatSink Sink;
    char         *Text;
    size_t        Kept;
    size_t        Room;      /* bytes that may still be kept, the NUL aside */
} BoxFormatBuffer;

static void BoxFormatBufferPut(BoxFormatSink *Sink, const char *Bytes, size_t Count)
{
    BoxFormatBuffer *Buffer = (BoxFormatBuffer *)Sink;
    size_t Step = Count < Buffer->Room ? Count : Buffer->Room;

    if (Step == 0)
        return;
    memcpy(Buffer->Text + Buffer->Kept, Bytes, Step);
    Buffer->Kept += Step;
    Buffer->Room -= Step;
}

int BoxFormatInto(char *Text, size_t Capacity, const char *Format, va_list Args)
{
    size_t Room = Text && Capacity != 0 ? Capacity - 1u : 0;
    BoxFormatBuffer Buffer = { { BoxFormatBufferPut, NULL }, Text, 0, Room };
    int Count = BoxFormat(&Buffer.Sink, Format, Args);

    if (Text && Capacity != 0)
        Text[Buffer.Kept] = '\0';
    return Count;
}
