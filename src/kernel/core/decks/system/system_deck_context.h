#ifndef SYSTEM_DECK_CONTEXT_H
#define SYSTEM_DECK_CONTEXT_H

#include "pocket.h"

#define MAX_CTX_USE_TAGS     3
#define CTX_TAG_LENGTH       64
#define CTX_USE_PARSE_BUF_SIZE 512

#define CTX_USE_ERR_SUCCESS          0x0000
#define CTX_USE_ERR_INVALID_FORMAT   0x0001
#define CTX_USE_ERR_TOO_MANY_TAGS    0x0002
#define CTX_USE_ERR_TAG_TOO_LONG     0x0003

typedef struct __packed {
    char context_string[CTX_USE_PARSE_BUF_SIZE];
} ctx_use_event_t;

typedef struct __packed {
    uint32_t error_code;
    char     message[128];
    uint8_t  reserved[60];
} ctx_use_response_t;

int system_deck_ctx_use(Pocket* pocket);

#endif // SYSTEM_DECK_CONTEXT_H
