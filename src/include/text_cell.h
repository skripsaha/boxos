#ifndef BOXOS_TEXT_CELL_H
#define BOXOS_TEXT_CELL_H

/*
 * One cell of the text console — a wire between programs and the display
 * program: a layer is a rectangle of these in shared memory (box/display.h).
 * The static assert below guards the size, because a stray field or a change
 * of order would be read as a screenful of the wrong colours rather than as
 * an error.
 *
 * Includer must provide uint*_t BEFORE including this header.
 *   Kernel:    #include "ktypes.h"
 *   Userspace: #include "box/types.h"
 */

typedef struct TextCell {
    uint32_t fg;        /* #RRGGBB */
    uint32_t bg;        /* #RRGGBB */
    uint32_t cp;        /* the character it shows: a Unicode codepoint, 0 for none —
                           never a glyph's number; which glyph draws it is the
                           display's to decide */
} TextCell;

#ifdef __cplusplus
static_assert(sizeof(TextCell) == 12,
              "TextCell is a kernel/userspace ABI struct — must stay 12 bytes");
#else
_Static_assert(sizeof(TextCell) == 12,
               "TextCell is a kernel/userspace ABI struct — must stay 12 bytes");
#endif

#endif /* BOXOS_TEXT_CELL_H */
