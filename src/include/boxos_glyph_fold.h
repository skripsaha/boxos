/*
 * boxos_glyph_fold.h — one contract on a character, held where the glyphs are.
 *
 * BoxOS speaks UTF-8 and a cell of the glass holds a character (text_cell.h).
 * A VGA text page draws a cell with the adapter's own 256-glyph CP437 font,
 * which cannot draw them all, so the fold from a codepoint to one of its
 * glyphs happens at the text page and nowhere upstream, in the two hands that
 * write one: the display program, a cell at a time (BoxGlyphFoldOne), and the
 * kernel's Emergency Screen, a line of text at a time (BoxGlyphFoldCodepoint).
 * One table, so a character looks the same on both. A framebuffer folds
 * nothing: its hands draw the character itself (boxos_imprint.h, Typecase).
 *
 * The table is the whole CP437 font read the other way — every character a
 * glyph of it draws — and, for the few that CP437 has no glyph of,
 * what the text was written with before it was written in Unicode: an em
 * dash as the rule, × as x, ∧ as ^, ≠ as != (two cells, so a line of text
 * folds it and a cell does not); and BoxOS's own characters
 * (boxos_symbols.h) as the pictures of the font nearest them — Duppo's two
 * halves a bubble and a dot, ○•. Sorted by codepoint and found by halves. A
 * codepoint that is none of these is drawn as 0xFE, the solid box: one cell,
 * visibly not a letter.
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
    {0x00A0, "\xFF"},
    {0x00A1, "\xAD"},
    {0x00A2, "\x9B"},
    {0x00A3, "\x9C"},
    {0x00A5, "\x9D"},
    {0x00A7, "\x15"},
    {0x00AA, "\xA6"},
    {0x00AB, "\xAE"},
    {0x00AC, "\xAA"},
    {0x00B0, "\xF8"},
    {0x00B1, "\xF1"},
    {0x00B2, "\xFD"},
    {0x00B5, "\xE6"},
    {0x00B6, "\x14"},
    {0x00B7, "\xFA"},
    {0x00BA, "\xA7"},
    {0x00BB, "\xAF"},
    {0x00BC, "\xAC"},
    {0x00BD, "\xAB"},
    {0x00BF, "\xA8"},
    {0x00C4, "\x8E"},
    {0x00C5, "\x8F"},
    {0x00C6, "\x92"},
    {0x00C7, "\x80"},
    {0x00C9, "\x90"},
    {0x00D1, "\xA5"},
    {0x00D6, "\x99"},
    {0x00D7, "x"},
    {0x00DC, "\x9A"},
    {0x00DF, "\xE1"},
    {0x00E0, "\x85"},
    {0x00E1, "\xA0"},
    {0x00E2, "\x83"},
    {0x00E4, "\x84"},
    {0x00E5, "\x86"},
    {0x00E6, "\x91"},
    {0x00E7, "\x87"},
    {0x00E8, "\x8A"},
    {0x00E9, "\x82"},
    {0x00EA, "\x88"},
    {0x00EB, "\x89"},
    {0x00EC, "\x8D"},
    {0x00ED, "\xA1"},
    {0x00EE, "\x8C"},
    {0x00EF, "\x8B"},
    {0x00F1, "\xA4"},
    {0x00F2, "\x95"},
    {0x00F3, "\xA2"},
    {0x00F4, "\x93"},
    {0x00F6, "\x94"},
    {0x00F7, "\xF6"},
    {0x00F9, "\x97"},
    {0x00FA, "\xA3"},
    {0x00FB, "\x96"},
    {0x00FC, "\x81"},
    {0x00FF, "\x98"},
    {0x0192, "\x9F"},
    {0x0393, "\xE2"},
    {0x0398, "\xE9"},
    {0x03A3, "\xE4"},
    {0x03A6, "\xE8"},
    {0x03A9, "\xEA"},
    {0x03B1, "\xE0"},
    {0x03B4, "\xEB"},
    {0x03B5, "\xEE"},
    {0x03C0, "\xE3"},
    {0x03C3, "\xE5"},
    {0x03C4, "\xE7"},
    {0x03C6, "\xED"},
    {0x2014, "\xC4"},
    {0x2022, "\x07"},
    {0x203C, "\x13"},
    {0x207F, "\xFC"},
    {0x20A7, "\x9E"},
    {0x2190, "\x1B"},
    {0x2191, "\x18"},
    {0x2192, "\x1A"},
    {0x2193, "\x19"},
    {0x2194, "\x1D"},
    {0x2195, "\x12"},
    {0x21A8, "\x17"},
    {0x2219, "\xF9"},
    {0x221A, "\xFB"},
    {0x221E, "\xEC"},
    {0x221F, "\x1C"},
    {0x2227, "^"},
    {0x2229, "\xEF"},
    {0x2248, "\xF7"},
    {0x2260, "!="},
    {0x2261, "\xF0"},
    {0x2264, "\xF3"},
    {0x2265, "\xF2"},
    {0x2302, "\x7F"},
    {0x2310, "\xA9"},
    {0x2320, "\xF4"},
    {0x2321, "\xF5"},
    {0x2500, "\xC4"},
    {0x2502, "\xB3"},
    {0x250C, "\xDA"},
    {0x2510, "\xBF"},
    {0x2514, "\xC0"},
    {0x2518, "\xD9"},
    {0x251C, "\xC3"},
    {0x2524, "\xB4"},
    {0x252C, "\xC2"},
    {0x2534, "\xC1"},
    {0x253C, "\xC5"},
    {0x2550, "\xCD"},
    {0x2551, "\xBA"},
    {0x2552, "\xD5"},
    {0x2553, "\xD6"},
    {0x2554, "\xC9"},
    {0x2555, "\xB8"},
    {0x2556, "\xB7"},
    {0x2557, "\xBB"},
    {0x2558, "\xD4"},
    {0x2559, "\xD3"},
    {0x255A, "\xC8"},
    {0x255B, "\xBE"},
    {0x255C, "\xBD"},
    {0x255D, "\xBC"},
    {0x255E, "\xC6"},
    {0x255F, "\xC7"},
    {0x2560, "\xCC"},
    {0x2561, "\xB5"},
    {0x2562, "\xB6"},
    {0x2563, "\xB9"},
    {0x2564, "\xD1"},
    {0x2565, "\xD2"},
    {0x2566, "\xCB"},
    {0x2567, "\xCF"},
    {0x2568, "\xD0"},
    {0x2569, "\xCA"},
    {0x256A, "\xD8"},
    {0x256B, "\xD7"},
    {0x256C, "\xCE"},
    {0x2580, "\xDF"},
    {0x2584, "\xDC"},
    {0x2588, "\xDB"},
    {0x258C, "\xDD"},
    {0x2590, "\xDE"},
    {0x2591, "\xB0"},
    {0x2592, "\xB1"},
    {0x2593, "\xB2"},
    {0x25A0, "\xFE"},
    {0x25AC, "\x16"},
    {0x25B2, "\x1E"},
    {0x25BA, "\x10"},
    {0x25BC, "\x1F"},
    {0x25C4, "\x11"},
    {0x25CB, "\x09"},
    {0x25D8, "\x08"},
    {0x25D9, "\x0A"},
    {0x263A, "\x01"},
    {0x263B, "\x02"},
    {0x263C, "\x0F"},
    {0x2640, "\x0C"},
    {0x2642, "\x0B"},
    {0x2660, "\x06"},
    {0x2663, "\x05"},
    {0x2665, "\x03"},
    {0x2666, "\x04"},
    {0x266A, "\x0D"},
    {0x266B, "\x0E"},
    {0x10B000, "\x09"},   /* Duppo, the left half: ○ */
    {0x10B001, "\x07"},   /* Duppo, the right half: • */
};

static inline int BoxGlyphFoldCodepoint(uint32_t Codepoint,
                                        char Out[BOXOS_GLYPH_FOLD_MAX])
{
    if (Codepoint < 0x80u) {
        Out[0] = (char)Codepoint;
        return 1;
    }

    unsigned Low = 0, High = sizeof(BoxGlyphFoldTable) / sizeof(BoxGlyphFoldTable[0]);
    while (Low < High) {
        unsigned Middle = Low + (High - Low) / 2;
        if (BoxGlyphFoldTable[Middle].Codepoint < Codepoint) {
            Low = Middle + 1;
            continue;
        }
        if (BoxGlyphFoldTable[Middle].Codepoint > Codepoint) {
            High = Middle;
            continue;
        }
        const char *Cells = BoxGlyphFoldTable[Middle].Cells;
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

/* The one glyph a cell showing Codepoint is drawn with. A cell is one place,
 * so what a line of text folds into two cells is the box here, as is a
 * control, which is no character to show; 0 is a cell that shows nothing. */
static inline unsigned char BoxGlyphFoldOne(uint32_t Codepoint)
{
    if (Codepoint == 0) return 0;
    if (Codepoint < 0x20u || Codepoint == 0x7Fu) return (unsigned char)BOXOS_GLYPH_FOLD_BOX;

    char Cells[BOXOS_GLYPH_FOLD_MAX];
    if (BoxGlyphFoldCodepoint(Codepoint, Cells) != 1) return (unsigned char)BOXOS_GLYPH_FOLD_BOX;
    return (unsigned char)Cells[0];
}

#endif /* BOXOS_GLYPH_FOLD_H */
