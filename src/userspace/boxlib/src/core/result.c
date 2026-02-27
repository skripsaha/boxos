#include "box/result.h"
#include "box/notify.h"
#include "box/system.h"

bool result_available(void) {
    result_page_t* rp = result_page();
    // CRITICAL FIX: Memory barrier to ensure fresh read of tail
    // Kernel updates tail with barriers (execution_deck.c:124-129)
    // Without barrier, userspace may see stale cached value -> miss new results
    __sync_synchronize();
    return rp->ring.head != rp->ring.tail;
}

uint32_t result_count(void) {
    result_page_t* rp = result_page();
    // CRITICAL FIX: Memory barrier to ensure fresh read of tail
    __sync_synchronize();
    uint32_t head = rp->ring.head;
    uint32_t tail = rp->ring.tail;

    if (tail >= head) {
        return tail - head;
    } else {
        return (BOX_RESULT_RING_SIZE - head) + tail;
    }
}

bool result_pop(result_entry_t* out_entry) {
    result_page_t* rp = result_page();

    if (!result_available()) {
        return false;
    }

    // Memory barrier to ensure fresh read
    __sync_synchronize();

    // Copy entry
    uint32_t head = rp->ring.head;
    *out_entry = rp->ring.entries[head];

    // Memory barrier BEFORE updating head
    __sync_synchronize();

    // Advance head
    rp->ring.head = (head + 1) % BOX_RESULT_RING_SIZE;

    return true;
}


bool result_page_full(void) {
    notify_page_t* np = notify_page();
    return np->result_page_full != 0;
}

bool event_ring_full(void) {
    notify_page_t* np = notify_page();
    return np->event_ring_full != 0;
}

uint32_t get_overflow_count(bool reset) {
    notify_page_t* np = notify_page();

    // Prepare notify page
    np->magic = BOX_NOTIFY_MAGIC;
    np->prefix_count = 1;
    np->flags = 0;
    np->status = 0;
    np->prefixes[0] = (0xFF << 8) | 0xE0;  // System Deck: GET_OVERFLOW_STATUS
    np->data[0] = reset ? 1 : 0;

    // Execute syscall
    __asm__ volatile("int $0x80");

    // Wait for result
    result_entry_t result;
    if (!result_wait(&result, 100000)) {
        return 0xFFFFFFFF;  // Timeout
    }

    // Parse result: data[0-3] = overflow_count, data[4] = overflow_flag
    uint32_t count = *((uint32_t*)result.payload);
    return count;
}
