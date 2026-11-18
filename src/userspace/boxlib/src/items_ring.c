/*
 * items_ring.c — a record placed in a ring of the items' language
 * (box/items.h): the one rule the program writing changes and the display
 * writing events both keep. Plain C of no calls, so the display's stand
 * builds it on the host as it is.
 */

#include "box/items.h"

uint8_t *item_ring_place(uint8_t *ring, uint32_t bytes, uint64_t *write, uint64_t read, uint32_t length)
{
    if (!ring || !write || bytes == 0 || bytes % 8u || length == 0 || length % 8u || length > bytes / 2u) return 0;
    uint64_t at = *write;
    /* Cursors that lie — the reader past the writer, or further behind than
     * the ring holds — leave no room at all. */
    if (at < read || at - read > bytes) return 0;
    const uint32_t offset = (uint32_t)(at % bytes);
    const uint32_t tail   = bytes - offset;
    const uint64_t need   = (uint64_t)length + (length > tail ? tail : 0u);
    if ((at - read) + need > bytes) return 0;
    if (length > tail) {
        /* The words up to the ring's end, passed over by the reader. */
        const ItemRecord pad = {ITEM_OP_PAD, 0, 0};
        __builtin_memcpy(ring + offset, &pad, sizeof(pad));
        at += tail;
        *write = at + length;
        return ring;
    }
    *write = at + length;
    return ring + offset;
}
