#include "pid_allocator.h"
#include "klib.h"
#include "io.h"

typedef struct {
    uint8_t bitmap[PID_BITMAP_SIZE];        // 32 bytes: allocation bitmap
    uint32_t generation[PID_MAX_COUNT];     // 1 KB: generation counters
    uint32_t allocated_count;
    spinlock_t lock;
} pid_allocator_t;

static pid_allocator_t g_allocator;

static inline void bitmap_set(uint8_t* bitmap, uint32_t index) {
    bitmap[index / 8] |= (1 << (index % 8));
}

static inline void bitmap_clear(uint8_t* bitmap, uint32_t index) {
    bitmap[index / 8] &= ~(1 << (index % 8));
}

static inline bool bitmap_test(const uint8_t* bitmap, uint32_t index) {
    return (bitmap[index / 8] & (1 << (index % 8))) != 0;
}

static uint32_t bitmap_find_free(const uint8_t* bitmap, uint32_t max_bits) {
    for (uint32_t i = 0; i < max_bits; i++) {
        if (!bitmap_test(bitmap, i)) {
            return i;
        }
    }
    return max_bits;  // not found
}

void pid_allocator_init(void) {
    memset(&g_allocator, 0, sizeof(pid_allocator_t));

    // generation 0 is reserved for PID_INVALID; start at 1
    for (uint32_t i = 0; i < PID_MAX_COUNT; i++) {
        g_allocator.generation[i] = 1;
    }

    spinlock_init(&g_allocator.lock);

    debug_printf("[PID] Allocator initialized\n");
    debug_printf("[PID]   Capacity: %u PIDs\n", PID_MAX_COUNT);
    debug_printf("[PID]   Memory: %zu bytes (bitmap) + %zu bytes (generations)\n",
                 sizeof(g_allocator.bitmap), sizeof(g_allocator.generation));
}

uint32_t pid_alloc(void) {
    spin_lock(&g_allocator.lock);

    uint32_t index = bitmap_find_free(g_allocator.bitmap, PID_MAX_COUNT);

    if (index >= PID_MAX_COUNT) {
        spin_unlock(&g_allocator.lock);
        debug_printf("[PID] ERROR: PID exhaustion (allocated=%u/%u)\n",
                     g_allocator.allocated_count, PID_MAX_COUNT);
        return PID_INVALID;
    }

    bitmap_set(g_allocator.bitmap, index);
    g_allocator.generation[index]++;

    if (g_allocator.generation[index] == 0) {
        // generation wrapped: uniqueness of PIDs can no longer be guaranteed
        debug_printf("[PID] CRITICAL: Generation overflow on index %u (2^24 allocations)\n", index);
        debug_printf("[PID] System has exhausted generation counter for slot %u\n", index);
        debug_printf("[PID] Cannot guarantee PID uniqueness - HALTING\n");
        spin_unlock(&g_allocator.lock);
        while (1) { asm volatile("cli; hlt"); }
    }

    uint32_t pid = PID_BUILD(g_allocator.generation[index], index);
    g_allocator.allocated_count++;

    spin_unlock(&g_allocator.lock);

    debug_printf("[PID] Allocated PID 0x%08x (gen=%u, idx=%u, count=%u/%u)\n",
                 pid, PID_TO_GEN(pid), index,
                 g_allocator.allocated_count, PID_MAX_COUNT);

    return pid;
}

void pid_free(uint32_t pid) {
    if (pid == PID_INVALID) {
        return;
    }

    uint32_t index = PID_TO_INDEX(pid);
    uint32_t gen = PID_TO_GEN(pid);

    if (index >= PID_MAX_COUNT) {
        debug_printf("[PID] ERROR: Invalid PID 0x%08x (index=%u out of range)\n",
                     pid, index);
        return;
    }

    spin_lock(&g_allocator.lock);

    if (!bitmap_test(g_allocator.bitmap, index)) {
        debug_printf("[PID] WARNING: Double-free detected for PID 0x%08x (index=%u)\n",
                     pid, index);
        spin_unlock(&g_allocator.lock);
        return;
    }

    // reject stale PIDs whose generation no longer matches the slot
    if (g_allocator.generation[index] != gen) {
        debug_printf("[PID] WARNING: Freeing stale PID 0x%08x (gen=%u, current gen=%u)\n",
                     pid, gen, g_allocator.generation[index]);
        spin_unlock(&g_allocator.lock);
        return;
    }

    bitmap_clear(g_allocator.bitmap, index);
    g_allocator.allocated_count--;

    spin_unlock(&g_allocator.lock);

    debug_printf("[PID] Freed PID 0x%08x (gen=%u, idx=%u, remaining=%u/%u)\n",
                 pid, gen, index,
                 g_allocator.allocated_count, PID_MAX_COUNT);
}

bool pid_validate(uint32_t pid) {
    if (pid == PID_INVALID) {
        return false;
    }

    uint32_t index = PID_TO_INDEX(pid);
    uint32_t gen = PID_TO_GEN(pid);

    if (index >= PID_MAX_COUNT) {
        return false;
    }

    spin_lock(&g_allocator.lock);

    bool allocated = bitmap_test(g_allocator.bitmap, index);
    bool gen_match = (g_allocator.generation[index] == gen);

    spin_unlock(&g_allocator.lock);

    return allocated && gen_match;
}

uint32_t pid_allocated_count(void) {
    spin_lock(&g_allocator.lock);
    uint32_t count = g_allocator.allocated_count;
    spin_unlock(&g_allocator.lock);
    return count;
}
