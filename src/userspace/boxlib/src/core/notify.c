#include "box/notify.h"
#include "box/result.h"
#include "box/string.h"

void pocket_prepare(Pocket* p) {
    memset(p, 0, sizeof(Pocket));
}

bool pocket_add_prefix(Pocket* p, uint8_t deck_id, uint8_t opcode) {
    if (!p || p->prefix_count >= MAX_PREFIXES) {
        return false;
    }
    p->prefixes[p->prefix_count++] = PREFIX(deck_id, opcode);
    return true;
}

void pocket_set_data(Pocket* p, void* data, uint32_t length) {
    if (!p) return;
    p->data_addr = (uint64_t)(uintptr_t)data;
    p->data_length = length;
}

int pocket_submit(Pocket* p) {
    if (!p) return -1;

    PocketRing* ring = pocket_ring();
    if (!pocket_ring_push(ring, p)) {
        return -1;  // ring full
    }

    __notify();

    return 0;
}

int pocket_send(uint8_t deck_id, uint8_t opcode, void* data, uint32_t length) {
    Pocket p;
    pocket_prepare(&p);
    pocket_add_prefix(&p, deck_id, opcode);
    if (data && length > 0) {
        pocket_set_data(&p, data, length);
    }
    return pocket_submit(&p);
}

/* =========================================================================
 * Batch API
 * ========================================================================= */

int pocket_queue(uint8_t deck_id, uint8_t opcode, void* data, uint32_t length) {
    Pocket p;
    pocket_prepare(&p);
    pocket_add_prefix(&p, deck_id, opcode);
    if (data && length > 0) {
        pocket_set_data(&p, data, length);
    }

    PocketRing* ring = pocket_ring();
    if (!pocket_ring_push(ring, &p)) {
        return -1;
    }
    return 0;
}

void pocket_flush(void) {
    __notify();
}

int pocket_flush_wait(void* results_ptr, int count, uint32_t timeout_ms) {
    __notify();

    Result *results = (Result *)results_ptr;
    int received = 0;
    for (int i = 0; i < count; i++) {
        if (!result_wait(&results[i], timeout_ms)) {
            break;
        }
        received++;
    }
    return received;
}
