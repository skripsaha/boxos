#ifndef VIDEO_OPS_H
#define VIDEO_OPS_H

#include "ktypes.h"

typedef struct {
    void     (*PrintChar)(char c, uint8_t attr);
    void     (*ScrollUp)(void);
    void     (*ClearScreen)(void);
    void     (*ClearLine)(int line);
    void     (*ClearToEol)(uint8_t attr);
    void     (*SetCursor)(int x, int y);
    int      (*GetCursorX)(void);
    int      (*GetCursorY)(void);
    void     (*UpdateCursor)(void);
    void     (*ChangeBackground)(uint8_t bg);
    uint16_t (*GetCols)(void);
    uint16_t (*GetRows)(void);
} DisplayOps;

#endif /* VIDEO_OPS_H */
