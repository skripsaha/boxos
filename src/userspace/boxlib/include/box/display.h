#ifndef BOX_DISPLAY_H
#define BOX_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"


#define DISP_CMD_LANE     0x30

#define DISP_CMD_LISTEN   0x13
#define DISP_CMD_PING     0x12

#define CONSOLE_RUN_TEXT      0x01
#define CONSOLE_RUN_CLEAR     0x02
#define CONSOLE_RUN_STEP      0x03

#define CONSOLE_RUN_TEXT_MAX  108u

typedef struct PACKED {
    uint8_t  kind;
    uint8_t  len;
    uint16_t _reserved;
    uint32_t fg;
    uint32_t bg;
    uint64_t order;
    char     text[CONSOLE_RUN_TEXT_MAX];
} ConsoleRun;

STATIC_ASSERT(sizeof(ConsoleRun) == 128,
              "ConsoleRun is the console-lane frame ABI — must stay 128 bytes");

#define CONSOLE_LANE_FRAMES   256u

#define CONSOLE_ORDER_TAG     "console:order"
#define CONSOLE_ORDER_BYTES   4096u

#ifdef __cplusplus
}
#endif

#endif