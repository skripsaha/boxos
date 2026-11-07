/* format_bridge — format.c compiled with BoxLib's own headers, and what
 * format_stand reaches of it in plain C types, so the stand itself can be
 * compiled with the host's. */

#include "../src/userspace/boxlib/src/format.c"

/* BoxLib's own vsnprintf, the one the stand holds to the host's. */
int BridgeFormat(char *Text, size_t Capacity, const char *Format, va_list Args)
{
    return BoxFormatInto(Text, Capacity, Format, Args);
}

/* One x87 long double, Mantissa x 2^Exponent, by %f, %e or %g. */
int BridgeExtended(char *Text, size_t Capacity, char Conversion, int Precision, int Alternate,
                   unsigned long long Mantissa, int Exponent, int Negative)
{
    static uint32_t Limb[BOX_FORMAT_LONG_LIMBS];
    static uint32_t Chunk[BOX_FORMAT_LONG_CHUNKS];
    BoxFormatBuffer Buffer = { { BoxFormatBufferPut, NULL }, Text, 0, Capacity - 1u };
    BoxFormatOut Out = { &Buffer.Sink, 0, 0 };
    BoxFormatSpec Spec;
    BoxFormatFloat Value;

    memset(&Spec, 0, sizeof(Spec));
    Spec.Conversion = Conversion;
    Spec.Precision = Precision;
    Spec.Alternate = (uint8_t)Alternate;
    Value.Negative = (uint8_t)Negative;
    Value.Kind = Mantissa == 0 ? BOX_FLOAT_ZERO : BOX_FLOAT_FINITE;
    Value.Bits = 64;
    Value.Mantissa = Mantissa;
    Value.Exponent = Exponent;
    BoxFormatFloating(&Out, &Spec, &Value, Limb, BOX_FORMAT_LONG_LIMBS, Chunk);
    Text[Buffer.Kept] = '\0';
    return (int)Out.Written;
}
