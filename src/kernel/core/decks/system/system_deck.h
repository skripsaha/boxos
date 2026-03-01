#ifndef SYSTEM_DECK_H
#define SYSTEM_DECK_H

#include "events.h"
#include "boxos_decks.h"

#define SYSTEM_DECK_ID           DECK_SYSTEM

#define SYSTEM_OP_PROC_SPAWN     0x01
#define SYSTEM_OP_PROC_KILL      0x02
#define SYSTEM_OP_PROC_INFO      0x03
#define SYSTEM_OP_PROC_EXEC      0x06

#define SYSTEM_OP_CTX_USE        0x04
#define SYSTEM_OP_BYPASS         0x05

#define SYSTEM_OP_BUF_ALLOC      0x10
#define SYSTEM_OP_BUF_FREE       0x11
#define SYSTEM_OP_BUF_RESIZE     0x12

#define SYSTEM_OP_TAG_ADD        0x20
#define SYSTEM_OP_TAG_REMOVE     0x21
#define SYSTEM_OP_TAG_CHECK      0x22

#define SYSTEM_OP_DEFRAG_FILE           0x18
#define SYSTEM_OP_FRAGMENTATION_SCORE   0x19

#define SYSTEM_OP_OVERFLOW_STATUS       0xE0

#define SYSTEM_OP_ROUTE          0x40
#define SYSTEM_OP_ROUTE_TAG      0x41
#define SYSTEM_OP_LISTEN         0x42

#define SYSTEM_OP_YIELD                 0xFE

typedef struct __packed {
    uint32_t file_id;
    uint32_t target_block;
} defrag_request_t;

typedef struct __packed {
    uint32_t file_id;
    uint32_t target_block;
    uint8_t reserved[184];
} defrag_file_event_t;

typedef struct __packed {
    uint32_t error_code;
    uint32_t fragmentation_score;
    uint8_t reserved[184];
} defrag_file_response_t;

typedef struct __packed {
    uint8_t reserved[192];
} fragmentation_score_event_t;

typedef struct __packed {
    uint32_t score;
    uint32_t total_files;
    uint32_t total_gaps;
    uint8_t reserved[180];
} fragmentation_score_response_t;

int system_deck_handler(Event* event);
bool system_security_gate(uint32_t pid, uint8_t deck_id, uint8_t opcode);

#endif // SYSTEM_DECK_H
