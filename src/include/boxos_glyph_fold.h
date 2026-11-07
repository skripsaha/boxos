/* Shared UTF-8-to-cell folding table.
 *
 * The kernel and the userspace display compositor must build the same text
 * surface.  Keeping the font decision in one small shared header prevents a
 * Unicode character from occupying different cells in the base layer and on
 * the final glass.  The includer supplies uint32_t and char types. */
#ifndef BOXOS_GLYPH_FOLD_H
#define BOXOS_GLYPH_FOLD_H

#define BOXOS_GLYPH_FOLD_BOX ((char)0xFE)
#define BOXOS_GLYPH_FOLD_MAX 2

typedef struct BoxGlyphFold {
    uint32_t    Codepoint;
    const char *Cells;
} BoxGlyphFold;

static const BoxGlyphFold BoxGlyphFoldTable[] = {
    {0x00A7, "\x15"},
    {0x00B1, "\xF1"},
    {0x203C, "\x13"},
    {0x2192, "\x1A"},
    {0x2194, "\x1D"},
    {0x2248, "\xF7"},
    {0x2014, "\xC4"},
    {0x00D7, "x"},
    {0x2227, "^"},
    {0x2265, ">="},
    {0x2260, "!="},
};

static inline int BoxGlyphFoldCodepoint(uint32_t Codepoint,
                                        char Out[BOXOS_GLYPH_FOLD_MAX])
{
    if (Codepoint < 0x80u) {
        Out[0] = (char)Codepoint;
        return 1;
    }

    for (unsigned Index = 0;
         Index < sizeof(BoxGlyphFoldTable) / sizeof(BoxGlyphFoldTable[0]);
         Index++) {
        if (BoxGlyphFoldTable[Index].Codepoint != Codepoint) continue;
        const char *Cells = BoxGlyphFoldTable[Index].Cells;
        int Count = 0;
        while (Count < BOXOS_GLYPH_FOLD_MAX && Cells[Count]) {
            Out[Count] = Cells[Count];
            Count++;
        }
        return Count;
    }

    Out[0] = BOXOS_GLYPH_FOLD_BOX;
    return 1;
}

#endif /* BOXOS_GLYPH_FOLD_H */
