#ifndef BOX_DISPLAY_H
#define BOX_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#define DISP_CMD_CLEAR    0x01
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
