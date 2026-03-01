#ifndef BOX_IO_KEYBOARD_H
#define BOX_IO_KEYBOARD_H

#include "../types.h"
#include "../error.h"

#define KB_OP_GETCHAR    0x60
#define KB_OP_READLINE   0x61
#define KB_OP_STATUS     0x62

#define KB_ERR_NO_DATA          ERR_WOULD_BLOCK
#define KB_ERR_ACCESS_DENIED    ERR_ACCESS_DENIED
#define KB_ERR_WOULD_BLOCK      ERR_WOULD_BLOCK
#define KB_ERR_INTERRUPTED      ERR_TIMEOUT

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
int kb_readline_async(char* buffer, size_t size, bool echo);

INLINE int kb_map_error(int32_t kernel_error) {
    switch (kernel_error) {
        case 0:  return 0;
        case -1: return -ERR_INVALID_ARGUMENT;
        case -2: return -ERR_WOULD_BLOCK;
        case -3: return -ERR_ACCESS_DENIED;
        case -4: return -ERR_WOULD_BLOCK;
        case -5: return -ERR_TIMEOUT;
        default: return -ERR_INTERNAL;
    }
}

#endif // BOX_IO_KEYBOARD_H
