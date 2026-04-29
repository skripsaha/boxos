#ifndef BOX_IO_KEYBOARD_H
#define BOX_IO_KEYBOARD_H

#include "../types.h"
#include "../error.h"

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

#endif // BOX_IO_KEYBOARD_H
