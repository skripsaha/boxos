#ifndef RESULT_H
#define RESULT_H

#include "ktypes.h"


typedef struct __packed {
    uint32_t error_code;
    uint32_t data_length;
    uint64_t data_addr;
    uint32_t sender_pid;
    uint32_t context;
} Result;

_Static_assert(sizeof(Result) == 24, "Result must be 24 bytes for ResultRing packing");

#endif