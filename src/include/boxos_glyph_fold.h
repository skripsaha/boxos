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

/* The character a glyph of the font draws, as UTF-8 — the fold read the other
 * way, for whatever was drawn as glyphs and must be said as text: a layer that
 * becomes part of the console's account goes into the log ring as the
 * characters the glass shows. The whole CP437 font, index by index; glyphs 00
 * and FF draw nothing and are said as a space. */
static inline const char *BoxGlyphUnicode(unsigned char Glyph)
{
    static const char *const Text[256] = {
        /* 00 */ " ", "\u263A", "\u263B", "\u2665", "\u2666", "\u2663", "\u2660", "\u2022",
        /* 08 */ "\u25D8", "\u25CB", "\u25D9", "\u2642", "\u2640", "\u266A", "\u266B", "\u263C",
        /* 10 */ "\u25BA", "\u25C4", "\u2195", "\u203C", "\u00B6", "\u00A7", "\u25AC", "\u21A8",
        /* 18 */ "\u2191", "\u2193", "\u2192", "\u2190", "\u221F", "\u2194", "\u25B2", "\u25BC",
        /* 20 */ " ", "!", "\"", "#", "$", "%", "&", "'",
        /* 28 */ "(", ")", "*", "+", ",", "-", ".", "/",
        /* 30 */ "0", "1", "2", "3", "4", "5", "6", "7",
        /* 38 */ "8", "9", ":", ";", "<", "=", ">", "?",
        /* 40 */ "@", "A", "B", "C", "D", "E", "F", "G",
        /* 48 */ "H", "I", "J", "K", "L", "M", "N", "O",
        /* 50 */ "P", "Q", "R", "S", "T", "U", "V", "W",
        /* 58 */ "X", "Y", "Z", "[", "\\", "]", "^", "_",
        /* 60 */ "`", "a", "b", "c", "d", "e", "f", "g",
        /* 68 */ "h", "i", "j", "k", "l", "m", "n", "o",
        /* 70 */ "p", "q", "r", "s", "t", "u", "v", "w",
        /* 78 */ "x", "y", "z", "{", "|", "}", "~", "\u2302",
        /* 80 */ "\u00C7", "\u00FC", "\u00E9", "\u00E2", "\u00E4", "\u00E0", "\u00E5", "\u00E7",
        /* 88 */ "\u00EA", "\u00EB", "\u00E8", "\u00EF", "\u00EE", "\u00EC", "\u00C4", "\u00C5",
        /* 90 */ "\u00C9", "\u00E6", "\u00C6", "\u00F4", "\u00F6", "\u00F2", "\u00FB", "\u00F9",
        /* 98 */ "\u00FF", "\u00D6", "\u00DC", "\u00A2", "\u00A3", "\u00A5", "\u20A7", "\u0192",
        /* A0 */ "\u00E1", "\u00ED", "\u00F3", "\u00FA", "\u00F1", "\u00D1", "\u00AA", "\u00BA",
        /* A8 */ "\u00BF", "\u2310", "\u00AC", "\u00BD", "\u00BC", "\u00A1", "\u00AB", "\u00BB",
        /* B0 */ "\u2591", "\u2592", "\u2593", "\u2502", "\u2524", "\u2561", "\u2562", "\u2556",
        /* B8 */ "\u2555", "\u2563", "\u2551", "\u2557", "\u255D", "\u255C", "\u255B", "\u2510",
        /* C0 */ "\u2514", "\u2534", "\u252C", "\u251C", "\u2500", "\u253C", "\u255E", "\u255F",
        /* C8 */ "\u255A", "\u2554", "\u2569", "\u2566", "\u2560", "\u2550", "\u256C", "\u2567",
        /* D0 */ "\u2568", "\u2564", "\u2565", "\u2559", "\u2558", "\u2552", "\u2553", "\u256B",
        /* D8 */ "\u256A", "\u2518", "\u250C", "\u2588", "\u2584", "\u258C", "\u2590", "\u2580",
        /* E0 */ "\u03B1", "\u00DF", "\u0393", "\u03C0", "\u03A3", "\u03C3", "\u00B5", "\u03C4",
        /* E8 */ "\u03A6", "\u0398", "\u03A9", "\u03B4", "\u221E", "\u03C6", "\u03B5", "\u2229",
        /* F0 */ "\u2261", "\u00B1", "\u2265", "\u2264", "\u2320", "\u2321", "\u00F7", "\u2248",
        /* F8 */ "\u00B0", "\u2219", "\u00B7", "\u221A", "\u207F", "\u00B2", "\u25A0", " ",
    };
    return Text[Glyph];
}

#endif /* BOXOS_GLYPH_FOLD_H */
