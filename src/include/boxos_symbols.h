/*
 * boxos_symbols.h — BoxOS's own characters. A character Unicode gives a
 * meaning is written as itself; one only BoxOS means — its mascot — takes a
 * place in the private use area of plane 16, from U+10B000 ("B0", a box),
 * where no font's icons stand. One table for every hand that writes or draws
 * them: a program writes the character, Typecase's SymbolSource draws it,
 * the fold (boxos_glyph_fold.h) shows it on a text page, glyphs lists it.
 *
 * Duppo stands two cells wide: its left half, then its right half.
 *
 * The includer supplies uint32_t.
 */
#ifndef BOXOS_SYMBOLS_H
#define BOXOS_SYMBOLS_H

#define BOXOS_SYMBOL_DUPPO_LEFT  0x10B000u
#define BOXOS_SYMBOL_DUPPO_RIGHT 0x10B001u

/* Duppo, both halves, in UTF-8: for a program that says it. */
#define BOXOS_SYMBOL_DUPPO_TEXT "\xF4\x8B\x80\x80\xF4\x8B\x80\x81"

typedef struct BoxSymbol {
    uint32_t    Character;
    const char *Name;
} BoxSymbol;

static const BoxSymbol BoxSymbols[] = {
    {BOXOS_SYMBOL_DUPPO_LEFT, "duppo, the left half"},
    {BOXOS_SYMBOL_DUPPO_RIGHT, "duppo, the right half"},
};

#endif /* BOXOS_SYMBOLS_H */
