#include "aslr.h"
#include "random.h"

/* A page-aligned offset in [0, max_bytes), each page as likely as the next
 * (RandomBelow): a biased draw would let a program spawned often enough narrow
 * the layout to the likelier end of the range. */
uint64_t aslr_random_offset(uint64_t max_bytes) {
    if (max_bytes < ASLR_PAGE_SIZE) return 0;
    return RandomBelow(max_bytes / ASLR_PAGE_SIZE) * ASLR_PAGE_SIZE;
}

aslr_offsets_t aslr_generate(void) {
    aslr_offsets_t offsets;
    offsets.stack_offset    = aslr_random_offset(ASLR_STACK_RANGE);
    offsets.heap_offset     = aslr_random_offset(ASLR_HEAP_RANGE);
    offsets.buf_heap_offset = aslr_random_offset(ASLR_BUF_RANGE);
    return offsets;
}
