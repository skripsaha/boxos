/* typecase stand — the imprint's digest as a program built on the header
 * tools/imprint writes says it: the very file the kernel and the display
 * include, read by the reader they read it with. typecase_stand.sh holds it
 * to the digest the stand says of the imprint it makes (SayImprintDigest). */

#include <stdint.h>
#include <stdio.h>

#include "boxos_symbols.h"
#include "boxos_imprint_data.h"

int main(void)
{
    const BoxImprint *Of = &BoxSystemImprint;
    printf("imprint %u", Of->Count);
    for (uint32_t Size = 0; Size < Of->SizeCount; Size++) {
        const BoxImprintSize *Cell = &Of->Sizes[Size];
        unsigned long long    Sum  = 0;
        for (uint32_t Glyph = 0; Glyph <= Of->Count; Glyph++)
            for (uint32_t Y = 0; Y < Cell->Height; Y++)
                for (uint32_t X = 0; X < Cell->Width; X++)
                    Sum += BoxImprintShade(Of, Size, Glyph, X, Y) * (Glyph % 7 + 1) * (X + 2 * Y + 1);
        printf(" %ux%u+%u@%u:%llu", Cell->Width, Cell->Height, Cell->Baseline, Cell->Offset, Sum);
    }
    printf(" find %u %u %u\n", BoxImprintFind(Of, 0x0416), BoxImprintFind(Of, BOXOS_SYMBOL_DUPPO_RIGHT),
           BoxImprintFind(Of, 0x4E00));
    return 0;
}
