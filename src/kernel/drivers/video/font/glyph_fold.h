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
#include "boxos_glyph_fold.h"

#define GLYPH_FOLD_BOX BOXOS_GLYPH_FOLD_BOX
#define GLYPH_FOLD_MAX BOXOS_GLYPH_FOLD_MAX

typedef BoxGlyphFold GlyphFold;

/* Fold one codepoint into the cells this font can draw. Writes 1..
 * GLYPH_FOLD_MAX bytes to `out` and returns how many — never zero, because a
 * character that reached the glass has to leave a mark there or the line on
 * the board stops matching the line in the log. */
static inline int GlyphFoldCodepoint(uint32_t cp, char out[GLYPH_FOLD_MAX])
{
    return BoxGlyphFoldCodepoint(cp, out);
}

#endif /* GLYPH_FOLD_H */
