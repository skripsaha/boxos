#ifndef BOXOS_TEXT_CELL_H
#define BOXOS_TEXT_CELL_H

/*
 * Shared kernel/userspace ABI header — one cell of the text console.
 *
 * The Canvas has always held the screen as an array of these; what is new is
 * that userspace may hand a rectangle of them over as one op (HW_VGA_PAINT,
 * box/vga.h vga_paint). So the layout stopped being the video driver's
 * private business and became a wire: kernel and userspace include THIS
 * header, and the static assert below guards the size, because a stray field
 * or a change of order would be read as a screenful of the wrong colours
 * rather than as an error.
 *
 * Includer must provide uint*_t BEFORE including this header.
 *   Kernel:    #include "ktypes.h"
 *   Userspace: #include "box/types.h"
 */

typedef struct TextCell {
    uint32_t fg;        /* #RRGGBB */
    uint32_t bg;        /* #RRGGBB */
    char     ch;
    uint8_t  pad[3];
} TextCell;

#ifdef __cplusplus
static_assert(sizeof(TextCell) == 12,
              "TextCell is a kernel/userspace ABI struct — must stay 12 bytes");
#else
_Static_assert(sizeof(TextCell) == 12,
               "TextCell is a kernel/userspace ABI struct — must stay 12 bytes");
#endif

#endif /* BOXOS_TEXT_CELL_H */
