/* format_stand — BoxLib's printf conversions held to the host's C library.
 *
 *   format_stand random N       N random conversions — integers, characters,
 *                               strings, pointers and every floating form with
 *                               random flags, widths and precisions — each
 *                               compared with the host's vsnprintf; then N/10
 *                               texts cut at random capacities, BoxLib's bound
 *                               held to the host's snprintf byte for byte
 *   format_stand extended FILE  x87 long double vectors from format_oracle.py:
 *                               the host has no 80-bit type to compare with
 *
 * format.c is reached through format_bridge.c, compiled with BoxLib's own
 * headers; this file has only the host's. Where the host does not write
 * what C asks, another witness stands in: %a at a precision is rounded here,
 * from the host's exact %a, to nearest with ties to even (the host drops the
 * digits instead); a NaN's sign, which the host never prints, is held to
 * fixed cases; and so is %g at a tie, where the host keeps a trailing zero C
 * says to drop (186020500 at %g is 1.8602e+08, not 1.86020e+08) — such host
 * answers are passed over and counted. %p of NULL and the flags C leaves
 * undefined are not generated. */

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define FORMAT_STAND_TEXT 65536u

int BridgeFormat(char *Text, size_t Capacity, const char *Format, va_list Args);
int BridgeExtended(char *Text, size_t Capacity, char Conversion, int Precision, int Alternate,
                   unsigned long long Mantissa, int Exponent, int Negative);

/* BoxLib's text and count for a format and its arguments. */
static char *Ours(int *Count, const char *Format, ...)
{
    char *Text = malloc(FORMAT_STAND_TEXT);
    va_list Args;

    va_start(Args, Format);
    *Count = BridgeFormat(Text, FORMAT_STAND_TEXT, Format, Args);
    va_end(Args);
    return Text;
}

/* BoxLib's vsnprintf at a capacity of the caller's. */
static int OursBounded(char *Text, size_t Capacity, const char *Format, ...)
{
    va_list Args;
    int Count;

    va_start(Args, Format);
    Count = BridgeFormat(Text, Capacity, Format, Args);
    va_end(Args);
    return Count;
}

static char *Theirs(int *Count, const char *Format, ...)
{
    va_list Args;
    char *Text;
    int Length;

    va_start(Args, Format);
    Length = vsnprintf(NULL, 0, Format, Args);
    va_end(Args);
    Text = malloc((size_t)Length + 1u);
    va_start(Args, Format);
    *Count = vsnprintf(Text, (size_t)Length + 1u, Format, Args);
    va_end(Args);
    return Text;
}

static uint64_t State = 0x9E3779B97F4A7C15u;

static uint64_t Random(void)
{
    State ^= State << 13;
    State ^= State >> 7;
    State ^= State << 17;
    return State;
}

static unsigned Below(unsigned Limit)
{
    return (unsigned)(Random() % Limit);
}

static double RandomDouble(void)
{
    static const double Edges[] = {
        0.0, 1.0, 0.5, 0.25, 0.125, 2.5, 0.05, 9.5, 99.5, 0.95, 1e22, 1e23, 5e-324,
        2.2250738585072014e-308, 1.7976931348623157e308, 123456789.0, 0.1, 9.999999,
        999999.5, 1e-5, 1e-4, 0.0001234, 1e15, 1e16, 1e17, 4503599627370496.5, 3.0,
    };
    double Value;
    uint64_t Bits;

    switch (Below(5)) {
    case 0:
        Value = Edges[Below(sizeof(Edges) / sizeof(Edges[0]))];
        break;
    case 1:                                   /* a few decimal digits, a scale */
        Value = (double)(int64_t)(Random() % 2000001u) * pow(10.0, (double)((int)Below(41) - 20));
        break;
    case 2:                                   /* a small integer and a half */
        Value = (double)(Random() % 100000u) + (Below(2) ? 0.5 : 0.0);
        break;
    default:                                  /* any finite pattern */
        do {
            Bits = Random();
            memcpy(&Value, &Bits, sizeof(Value));
        } while (!isfinite(Value));
        break;
    }
    return Below(2) ? -Value : Value;
}

static int HexValue(char Digit)
{
    return Digit <= '9' ? Digit - '0' : (Digit | 0x20) - 'a' + 10;
}

/* %a at Precision, from the host's exact %a: the hex digits past the
 * precision compared with half of one, a tie going to the even digit. */
static char *HexRounded(double Value, int Precision, int Upper)
{
    static const char *Lower = "0123456789abcdef";
    size_t Room = (size_t)Precision + 64u;
    char Exact[64];
    char *Kept = malloc(Room);
    char *Text = malloc(Room + 32u);
    const char *At = Exact;
    int Negative = 0;
    int Lead;
    int Count = 0;
    int Index;
    int Up = 0;
    const char *Exponent;

    snprintf(Exact, sizeof(Exact), "%a", Value);
    if (*At == '-') {
        Negative = 1;
        At++;
    }
    At += 2;                                         /* 0x */
    Lead = HexValue(*At++);
    if (*At == '.')
        At++;
    while (*At != 'p')
        Kept[Count++] = *At++;
    Exponent = At + 1;
    if (Precision < Count) {
        int Half = HexValue(Kept[Precision]);
        int Rest = 0;

        for (Index = Precision + 1; Index < Count; Index++)
            Rest |= Kept[Index] != '0';
        if (Half > 8 || (Half == 8 && Rest))
            Up = 1;
        else if (Half == 8)
            Up = (Precision == 0 ? Lead : HexValue(Kept[Precision - 1])) & 1;
        Count = Precision;
        for (Index = Count - 1; Up && Index >= 0; Index--) {
            int Digit = HexValue(Kept[Index]) + 1;

            Kept[Index] = Lower[Digit & 0xF];
            Up = Digit == 16;
        }
        Lead += Up;
    }
    while (Count < Precision)
        Kept[Count++] = '0';
    Kept[Count] = '\0';
    snprintf(Text, Room + 32u, "[%s0x%d%s%s%c%s]", Negative ? "-" : "", Lead,
             Precision ? "." : "", Kept, 'p', Exponent);
    free(Kept);
    if (Upper)
        for (Index = 0; Text[Index]; Index++)
            if (Text[Index] >= 'a' && Text[Index] <= 'z')
                Text[Index] = (char)(Text[Index] - 'a' + 'A');
    return Text;
}

static void Flags(char *Spec, size_t *At, const char *Allowed)
{
    for (; *Allowed; Allowed++)
        if (Below(4) == 0)
            Spec[(*At)++] = *Allowed;
}

/* Whether the host kept a trailing zero in %g's fraction, which C drops. */
static int HostKeptZero(const char *Format, const char *Text)
{
    const char *Conversion = Format + strlen(Format) - 2u;
    const char *At;

    if ((*Conversion != 'g' && *Conversion != 'G') || strchr(Format, '#'))
        return 0;
    if (!strchr(Text, '.'))
        return 0;
    for (At = strchr(Text, '.') + 1; *At >= '0' && *At <= '9'; At++)
        ;
    return At[-1] == '0';
}

static int Compare(const char *Format, const char *Ours_, int OurCount, const char *Theirs_,
                   int TheirCount, unsigned long *Failed)
{
    if (OurCount == TheirCount && strcmp(Ours_, Theirs_) == 0)
        return 1;
    if (++*Failed <= 20)
        printf("  %s: ours [%s] (%d), host [%s] (%d)\n", Format, Ours_, OurCount, Theirs_, TheirCount);
    return 0;
}

static int RandomRun(unsigned long Count)
{
    static const char *Integers = "diouxX";
    static const char *Floats = "fFeEgGaA";
    static const char *Lengths[] = { "", "hh", "h", "l", "ll", "j", "z", "t" };
    unsigned long Failed = 0;
    unsigned long HostWrong = 0;
    unsigned long Index;

    for (Index = 0; Index < Count; Index++) {
        char Format[64];
        size_t At = 0;
        int Width = -1;
        int Precision = -1;
        int StarWidth = Below(8) == 0;
        int StarPrecision = Below(8) == 0;
        unsigned Kind = Below(10);
        int Floating = 0;
        double Shown = 0;
        char *A;
        char *B;
        int CountA;
        int CountB;

        Format[At++] = '[';
        Format[At++] = '%';
        if (Kind < 6) {                                   /* a floating form */
            char Conversion = Floats[Below(8)];
            double Value = RandomDouble();

            Floating = 1;
            Shown = Value;
            Flags(Format, &At, "-+ #0");
            if (!StarWidth && Below(2))
                At += (size_t)sprintf(Format + At, "%u", Below(40));
            else if (StarWidth)
                Format[At++] = '*';
            if (Below(3)) {
                Precision = Below(10) == 0 ? (int)Below(400) : (int)Below(25);
                if (Below(40) == 0 && (Conversion == 'f' || Conversion == 'F'))
                    Precision = 1000 + (int)Below(200);
            }
            if (StarPrecision && Precision >= 0)
                At += (size_t)sprintf(Format + At, ".*");
            else if (Precision >= 0)
                At += (size_t)sprintf(Format + At, ".%d", Precision);
            if ((Conversion == 'a' || Conversion == 'A') && Precision >= 0) {
                char Plain[32];

                snprintf(Plain, sizeof(Plain), "[%%.%d%c]", Precision, Conversion);
                A = Ours(&CountA, Plain, Value);
                B = HexRounded(Value, Precision, Conversion == 'A');
                CountB = (int)strlen(B);
                Compare(Plain, A, CountA, B, CountB, &Failed);
                free(A);
                free(B);
                continue;
            }
            Format[At++] = Conversion;
            Format[At++] = ']';
            Format[At] = '\0';
            Width = (int)Below(40) - 10;
            if (StarWidth && StarPrecision && Precision >= 0) {
                A = Ours(&CountA, Format, Width, Precision, Value);
                B = Theirs(&CountB, Format, Width, Precision, Value);
            } else if (StarWidth) {
                A = Ours(&CountA, Format, Width, Value);
                B = Theirs(&CountB, Format, Width, Value);
            } else if (StarPrecision && Precision >= 0) {
                A = Ours(&CountA, Format, Precision, Value);
                B = Theirs(&CountB, Format, Precision, Value);
            } else {
                A = Ours(&CountA, Format, Value);
                B = Theirs(&CountB, Format, Value);
            }
        } else if (Kind < 9) {                            /* an integer */
            char Conversion = Integers[Below(6)];
            const char *Length = Lengths[Below(8)];
            long long Value = (long long)Random() >> Below(64);

            Flags(Format, &At, Conversion == 'd' || Conversion == 'i' ? "-+ 0" : "-#0");
            if (Below(2))
                At += (size_t)sprintf(Format + At, "%u", Below(30));
            if (Below(2))
                At += (size_t)sprintf(Format + At, ".%u", Below(25));
            At += (size_t)sprintf(Format + At, "%s%c]", Length, Conversion);
            A = Ours(&CountA, Format, Value);
            B = Theirs(&CountB, Format, Value);
        } else {                                          /* a character, string, pointer */
            static const char *Strings[] = { "", "box", "BoxOS tags", "a longer string of text" };
            unsigned Which = Below(3);

            Flags(Format, &At, "-");
            if (Below(2))
                At += (size_t)sprintf(Format + At, "%u", Below(30));
            if (Which == 1 && Below(2))
                At += (size_t)sprintf(Format + At, ".%u", Below(12));
            Format[At++] = Which == 0 ? 'c' : Which == 1 ? 's' : 'p';
            Format[At++] = ']';
            Format[At] = '\0';
            if (Which == 0) {
                int Ch = 32 + (int)Below(95);
                A = Ours(&CountA, Format, Ch);
                B = Theirs(&CountB, Format, Ch);
            } else if (Which == 1) {
                const char *Text = Strings[Below(4)];
                A = Ours(&CountA, Format, Text);
                B = Theirs(&CountB, Format, Text);
            } else {
                void *Pointer = (void *)(uintptr_t)(Random() | 1u);
                A = Ours(&CountA, Format, Pointer);
                B = Theirs(&CountB, Format, Pointer);
            }
        }
        if (Floating && HostKeptZero(Format, B))
            HostWrong++;
        else if (!Compare(Format, A, CountA, B, CountB, &Failed) && Floating && Failed <= 20)
            printf("    the value: %.17g (%a)\n", Shown, Shown);
        free(A);
        free(B);
    }
    printf("random: %lu conversions, %lu differ; %lu %%g ties the host gets wrong passed over\n",
           Count, Failed, HostWrong);
    return Failed == 0;
}

/* The cases the host does not decide as C does, held to what C says. */
static int FixedCases(void)
{
    struct { const char *Format; double Value; const char *Want; } Cases[] = {
        { "%f", NAN, "nan" }, { "%+F", NAN, "+NAN" }, { "% e", NAN, " nan" },
        { "%f", -NAN, "-nan" }, { "%08.2f", -INFINITY, "    -inf" },
        { "%.0a", 1.5, "0x2p+0" }, { "%.0a", 2.5, "0x1p+1" }, { "%.1a", 1.96875, "0x2.0p+0" },
        { "%.0a", 1.75, "0x2p+0" }, { "%g", 186020500.0, "1.8602e+08" },
        { "%G", -15462050000000.0, "-1.5462E+13" }, { "%.3g", 2.5e-5, "2.5e-05" },
        { "%#g", 186020500.0, "1.86020e+08" }, { "%.1g", 0.25, "0.2" }, { "%.1g", 0.35, "0.3" },
    };
    unsigned long Failed = 0;
    size_t Index;

    for (Index = 0; Index < sizeof(Cases) / sizeof(Cases[0]); Index++) {
        int Count;
        char *Text = Ours(&Count, Cases[Index].Format, Cases[Index].Value);

        Compare(Cases[Index].Format, Text, Count, Cases[Index].Want,
                (int)strlen(Cases[Index].Want), &Failed);
        free(Text);
    }
    {
        int Count;
        int Written = -1;
        char *Text = Ours(&Count, "%s%n|%ls|%lc", "abc", &Written, L"Boç", (unsigned)0x1F4E6);

        Compare("%n %ls %lc", Text, Count, "abc|Bo\xc3\xa7|\xf0\x9f\x93\xa6", 13, &Failed);
        if (Written != 3 && ++Failed <= 20)
            printf("  %%n stored %d, not 3\n", Written);
        free(Text);
    }
    printf("fixed: %zu cases, %lu differ\n", sizeof(Cases) / sizeof(Cases[0]) + 1u, Failed);
    return Failed == 0;
}

/* Lines "conversion precision alternate mantissa exponent negative text". */
static int ExtendedRun(const char *Path)
{
    static char Text[FORMAT_STAND_TEXT];
    FILE *File = fopen(Path, "r");
    char *Line = NULL;
    size_t Capacity = 0;
    unsigned long Count = 0;
    unsigned long Failed = 0;

    if (!File) {
        perror(Path);
        return 0;
    }
    while (getline(&Line, &Capacity, File) > 0) {
        char Conversion;
        int Precision;
        int Alternate;
        unsigned long long Mantissa;
        int Exponent;
        int Negative;
        char *Want = malloc(strlen(Line) + 1u);

        if (sscanf(Line, " %c %d %d %llx %d %d %s", &Conversion, &Precision, &Alternate,
                   &Mantissa, &Exponent, &Negative, Want) != 7) {
            free(Want);
            continue;
        }
        BridgeExtended(Text, sizeof(Text), Conversion, Precision, Alternate, Mantissa, Exponent,
                       Negative);
        Count++;
        if (strcmp(Text, Want) != 0 && ++Failed <= 20)
            printf("  %%.%d%c of %llx x 2^%d: ours [%.80s], exact [%.80s]\n", Precision,
                   Conversion, Mantissa, Exponent, Text, Want);
        free(Want);
    }
    free(Line);
    fclose(File);
    printf("extended: %lu long double conversions, %lu differ\n", Count, Failed);
    return Count != 0 && Failed == 0;
}

/* snprintf's bound: the text cut to Capacity - 1 bytes and ended there, no
 * byte past Capacity written, nothing at all at a Capacity of 0 — where the
 * buffer may be NULL — and the length of the whole text returned either way.
 * Both buffers start filled with one mark, and are compared whole. */
static int BoundRun(unsigned long Count)
{
    static const char *Strings[] = { "", "box", "BoxOS tags", "a longer string of text" };
    unsigned long Failed = 0;
    unsigned long Index;

    for (Index = 0; Index < Count; Index++) {
        const char *Text = Strings[Below(4)];
        long long Number = (long long)Random() >> Below(64);
        double Value = RandomDouble();
        int Width = (int)Below(30);
        int Precision = (int)Below(12);
        int Full = snprintf(NULL, 0, "[%*s|%lld|%.*e]", Width, Text, Number, Precision, Value);
        size_t Capacity = Below((unsigned)Full + 3u);
        int Absent = Capacity == 0 && Below(2) == 0;
        char A[160];
        char B[160];
        int CountA;
        int CountB;

        memset(A, 0x5A, sizeof(A));
        memset(B, 0x5A, sizeof(B));
        CountA = OursBounded(Absent ? NULL : A, Capacity, "[%*s|%lld|%.*e]", Width, Text, Number,
                             Precision, Value);
        CountB = snprintf(Absent ? NULL : B, Capacity, "[%*s|%lld|%.*e]", Width, Text, Number,
                          Precision, Value);
        if (CountA == CountB && memcmp(A, B, sizeof(A)) == 0)
            continue;
        if (++Failed <= 20)
            printf("  capacity %zu of %d: ours [%.*s] (%d), host [%.*s] (%d)\n", Capacity, Full,
                   (int)Capacity, A, CountA, (int)Capacity, B, CountB);
    }
    printf("bound: %lu texts cut at random capacities, %lu differ\n", Count, Failed);
    return Failed == 0;
}

int main(int Count, char **Words)
{
    if (Count == 3 && strcmp(Words[1], "random") == 0) {
        unsigned long Conversions = strtoul(Words[2], NULL, 10);

        return RandomRun(Conversions) && FixedCases() && BoundRun(Conversions / 10) ? 0 : 1;
    }
    if (Count == 3 && strcmp(Words[1], "extended") == 0)
        return ExtendedRun(Words[2]) ? 0 : 1;
    fputs("usage: format_stand random N | extended FILE\n", stderr);
    return 2;
}
