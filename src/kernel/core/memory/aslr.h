#ifndef ASLR_H
#define ASLR_H

#include "ktypes.h"


#define ASLR_STACK_RANGE    (8ULL * 1024 * 1024)
#define ASLR_HEAP_RANGE     (16ULL * 1024 * 1024)
#define ASLR_BUF_RANGE      (16ULL * 1024 * 1024)

#define ASLR_PAGE_SIZE      4096

typedef struct {
    uint64_t stack_offset;
    uint64_t heap_offset;
    uint64_t buf_heap_offset;
} aslr_offsets_t;

void aslr_init(void);

aslr_offsets_t aslr_generate(void);

uint64_t aslr_random_offset(uint64_t max_bytes);

#endif