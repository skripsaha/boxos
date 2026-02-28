#ifndef SYSTEM_DECK_DEFRAG_H
#define SYSTEM_DECK_DEFRAG_H

#include "events.h"

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

int system_deck_defrag_file(Event* event);
int system_deck_fragmentation_score(Event* event);

#endif // SYSTEM_DECK_DEFRAG_H
