#ifndef BOX_DISPLAY_H
#define BOX_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Byte-stream records the display daemon renders. Colour is metadata, a
 * full pair of #RRGGBB values — never an escape sequence in the text. */
#define DISP_CMD_CLEAR    0x01
/* DISP_CMD_COLOR is followed by [u32 fg][u32 bg], little-endian. */
#define DISP_CMD_COLOR    0x02

#define DISP_CMD_READLINE 0x10
#define DISP_CMD_GETCHAR  0x11
#define DISP_CMD_PING     0x12

#define DISP_CMD_FOCUS    0x20
#define DISP_CMD_UNFOCUS  0x21

#ifdef __cplusplus
}
#endif

#endif
