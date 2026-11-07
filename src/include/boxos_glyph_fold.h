/*
 * boxos_glyph_fold.h — one contract on a character, held where the glyphs are.
 *
 * BoxOS speaks UTF-8: the log ring keeps the bytes it is given, the serial
 * Wire carries them and `logsave` writes them down. The glass is the one
 * surface that cannot take them — a cell holds ONE byte, and the 256-glyph
 * 8×16 font in boxos_font.h decides what it looks like. So the fold from a
 * codepoint to something drawable happens at the font and nowhere upstream,
 * in the two hands that draw: the display program and the kernel's Emergency
 * Screen. One table, so a character occupies the same cells on both.
 *
 * Each entry is either the very character at that CP437 index of the font, or
 * the ASCII the text was written with before it was written in Unicode. A
 * codepoint that is neither is drawn as 0xFE, the solid box: one cell,
 * visibly not a letter. The list is what the tree prints, not what CP437
 * holds; a codepoint nobody prints costs one boxed cell the first time
 * somebody does, and that box is the notice to add it.
 *
 * The includer supplies uint32_t and char types.
 */
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
