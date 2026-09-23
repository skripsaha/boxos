/*
 * glyph_fold.h — one contract on a character, held where the glyphs are.
 *
 * BoxOS speaks UTF-8 and always did: kprintf's own %[U] escape encodes a
 * codepoint into UTF-8 bytes and hands them to the same emitter as every
 * other character (klib_print.c), the log ring stores the bytes it is given,
 * the serial Wire carries them, `logsave` writes them to the volume, and a
 * watch.log read on another machine shows them correctly. The glass is the
 * one surface that cannot take them. Both backends put ONE byte in a cell and
 * let a 256-glyph 8×16 IBM VGA ROM font decide what it looks like — fb_gop.c
 * render_glyph indexes vga_font_8x16 by the cell byte, hw_vga.c op_DrawCells
 * writes the cell byte into VRAM for the hardware's own copy of that font.
 *
 * So the fold from a codepoint to something drawable belongs HERE, at the
 * font, and nowhere upstream. Before this file there was no fold at all and
 * two printers each guessed: the kernel wrote its UTF-8 bytes through
 * untouched, so an em dash became three empty cells on the board while the
 * log file looked right; boxlib replaced every multi-byte sequence with a
 * single '?', so `me` printed "6 cores ? 2 K-Core" and destroyed the
 * character in the log as well. The same em dash, drawn two wrong ways,
 * because the decision was taken twice by code that cannot see the font.
 *
 * Each entry below is one of two kinds and says which:
 *   SAME      — CP437 has this very character at the index named, and the
 *               font in vga_font.h draws it (the line is cited).
 *   STANDS IN — CP437 has no such character; these are the ASCII bytes the
 *               text was written with before it was written in Unicode.
 * A codepoint that is neither is drawn as 0xFE, the solid box: one cell,
 * visibly not a letter, so a character this machine cannot draw can never be
 * misread as one it can.
 *
 * ‼ THE LIST IS WHAT THE TREE PRINTS, NOT WHAT CP437 HOLDS. Measured over
 * every string literal under src/ with comments stripped: the kernel prints
 * exactly ten non-ASCII codepoints (— ×729, § ×49, → ×37, ‼ ×10, ∧ ×7, × ×2,
 * ≥ ×1, ± ×1, ↔ ×1, ≠ ×1) and userspace prints those plus the Unicode
 * conformance strings of apps/cxxtest.cpp, which no 256-glyph font can hold
 * and which are meant to be seen as unrepresentable. This must not grow into
 * a general CP437 table: a codepoint nobody prints costs one boxed cell the
 * first time somebody does, and that box is the notice to add it.
 */

#ifndef GLYPH_FOLD_H
#define GLYPH_FOLD_H

#include "ktypes.h"

/* CP437 0xFE, the solid box — vga_font.h:278. */
#define GLYPH_FOLD_BOX ((char)0xFE)

/* Longest run any entry below produces. */
#define GLYPH_FOLD_MAX 2

typedef struct GlyphFold {
    uint32_t    cp;
    const char *cells;   /* 1..GLYPH_FOLD_MAX bytes, each a CP437 index */
} GlyphFold;

static const GlyphFold glyph_fold_table[] = {
    /* ── SAME: CP437 holds this character ── */
    { 0x00A7, "\x15" },   /* § SECTION SIGN           — vga_font.h:41  */
    { 0x00B1, "\xF1" },   /* ± PLUS-MINUS SIGN        — vga_font.h:265 */
    { 0x203C, "\x13" },   /* ‼ DOUBLE EXCLAMATION     — vga_font.h:39  */
    { 0x2192, "\x1A" },   /* → RIGHTWARDS ARROW       — vga_font.h:46  */
    { 0x2194, "\x1D" },   /* ↔ LEFT RIGHT ARROW       — vga_font.h:49  */
    { 0x2248, "\xF7" },   /* ≈ ALMOST EQUAL TO        — vga_font.h:271 */

    /* ── STANDS IN: CP437 has no such character ── */

    /* — EM DASH → 0xC4, the box-drawing horizontal (vga_font.h:220). The
     * only glyph in this font that is a rule the full width of the cell,
     * which is what an em dash is and what it is used for in this log. */
    { 0x2014, "\xC4" },

    /* × MULTIPLICATION SIGN → the letter these phrases used before they were
     * spelled with a Unicode sign: "stress 64 x 8 tags". */
    { 0x00D7, "x"    },

    /* ∧ LOGICAL AND → its ASCII spelling. */
    { 0x2227, "^"    },

    /* ≥ and ≠ → two cells each. CP437 does have ≥ at 0xF2, but that slot is
     * sixteen zero bytes in vga_font.h, so the GOP renderer would draw a
     * blank where the VGA ROM draws a sign — and this font exists precisely
     * so both backends show the same thing (vga_font.h:1-9). CP437 has no ≠
     * at any index. Both are therefore spelled, and a spelled comparison is
     * one cell wider than the sign, which is the honest cost. */
    { 0x2265, ">="   },
    { 0x2260, "!="   },
};

/* Fold one codepoint into the cells this font can draw. Writes 1..
 * GLYPH_FOLD_MAX bytes to `out` and returns how many — never zero, because a
 * character that reached the glass has to leave a mark there or the line on
 * the board stops matching the line in the log. */
static inline int GlyphFoldCodepoint(uint32_t cp, char out[GLYPH_FOLD_MAX])
{
    if (cp < 0x80u) { out[0] = (char)cp; return 1; }

    for (unsigned i = 0; i < sizeof(glyph_fold_table) / sizeof(glyph_fold_table[0]); i++) {
        if (glyph_fold_table[i].cp != cp) continue;
        const char *s = glyph_fold_table[i].cells;
        int n = 0;
        while (n < GLYPH_FOLD_MAX && s[n]) { out[n] = s[n]; n++; }
        return n;
    }

    out[0] = GLYPH_FOLD_BOX;
    return 1;
}

#endif /* GLYPH_FOLD_H */
