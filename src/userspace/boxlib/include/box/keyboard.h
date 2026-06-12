#ifndef BOX_KEYBOARD_H
#define BOX_KEYBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"

typedef struct {
    char    ch;
    uint8_t scancode;
    uint8_t flags;
    uint8_t reserved;
} kb_char_t;

typedef struct {
    uint32_t available;
    uint32_t buffer_size;
} kb_status_t;

int kb_getchar(void);
int kb_readline(char* buffer, size_t size, bool echo);
int kb_status(kb_status_t* status);

int kb_getchar_timeout(uint32_t timeout_ms);
int kb_getchar_ex(kb_char_t* out_char);

#ifdef __cplusplus
}
#endif

#endif // BOX_KEYBOARD_H
