#ifndef BOXOS_TEXT_CELL_H
#define BOXOS_TEXT_CELL_H


typedef struct TextCell {
    uint32_t fg;
    uint32_t bg;
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

#endif