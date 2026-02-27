#ifndef BOX_IO_KEYBOARD_H
#define BOX_IO_KEYBOARD_H

#include "../types.h"
#include "../error.h"

// Hardware Deck Keyboard Opcodes (0x60-0x62)
#define KB_OP_GETCHAR    0x60
#define KB_OP_READLINE   0x61
#define KB_OP_STATUS     0x62

// Error codes (using standard error codes)
#define KB_ERR_NO_DATA          BOX_ERR_WOULD_BLOCK
#define KB_ERR_ACCESS_DENIED    BOX_ERR_ACCESS_DENIED
#define KB_ERR_WOULD_BLOCK      BOX_ERR_WOULD_BLOCK
#define KB_ERR_INTERRUPTED      BOX_ERR_TIMEOUT

// Extended character info
typedef struct {
    char    ch;
    uint8_t scancode;
    uint8_t flags;
    uint8_t reserved;
} box_kb_char_t;

// Keyboard buffer status
typedef struct {
    uint32_t available;
    uint32_t buffer_size;
} box_kb_status_t;

// Core API Functions
int kb_getchar(void);
int kb_readline(char* buffer, size_t size, bool echo);
int kb_status(box_kb_status_t* status);

// Extended API Functions
int kb_getchar_timeout(uint32_t timeout_ms);
int kb_getchar_ex(box_kb_char_t* out_char);
int kb_readline_async(char* buffer, size_t size, bool echo);

// Helper: Map kernel error codes to BoxLib error codes
BOX_INLINE int kb_map_error(int32_t kernel_error) {
    switch (kernel_error) {
        case 0:  return 0;
        case -1: return -BOX_ERR_INVALID_ARGUMENT;
        case -2: return -BOX_ERR_WOULD_BLOCK;
        case -3: return -BOX_ERR_ACCESS_DENIED;
        case -4: return -BOX_ERR_WOULD_BLOCK;
        case -5: return -BOX_ERR_TIMEOUT;
        default: return -BOX_ERR_INTERNAL;
    }
}

#endif // BOX_IO_KEYBOARD_H
