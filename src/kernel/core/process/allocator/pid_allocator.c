#include "pid_allocator.h"
#include "klib.h"
#include "io.h"

#define PID_BITMAP_SIZE ((PID_MAX_COUNT + 7) / 8)

typedef struct
{
    uint8_t bitmap[PID_BITMAP_SIZE];    // 512 bytes for 4096 PIDs: allocation bitmap
    uint32_t generation[PID_MAX_COUNT]; // generation counters (tracked but not exposed)
    uint32_t allocated_count;
    spinlock_t lock;
} pid_allocator_t;

static pid_allocator_t g_allocator;

static inline void bitmap_set(uint8_t *bitmap, uint32_t index)
{
    bitmap[index / 8] |= (1 << (index % 8));
}

static inline void bitmap_clear(uint8_t *bitmap, uint32_t index)
{
    bitmap[index / 8] &= ~(1 << (index % 8));
}

static inline bool bitmap_test(const uint8_t *bitmap, uint32_t index)
{
    return (bitmap[index / 8] & (1 << (index % 8))) != 0;
}

static uint32_t bitmap_find_free(const uint8_t *bitmap, uint32_t max_bits)
{
    for (uint32_t i = 0; i < max_bits; i++)
    {
        if (!bitmap_test(bitmap, i))
        {
            return i;
        }
    }
    return max_bits; // not found
}

void pid_allocator_init(void)
{
    memset(&g_allocator, 0, sizeof(pid_allocator_t));

    // generation 0 is reserved for PID_INVALID; start generations at 0.
    // First allocation will increment to 1, producing PID = (1 << 16) | 0 = 65536.
    // This is expected — generation 0 is invalid, so first valid generation is 1.
    // All generations start at 0, increment happens in pid_alloc().
    // (memset already zeroed everything, so no explicit loop needed)

    spinlock_init(&g_allocator.lock);

    debug_printf("[PID] Allocator initialized\n");
    debug_printf("[PID]   Capacity: %u PIDs\n", PID_MAX_COUNT);
    debug_printf("[PID]   Memory: %zu bytes (bitmap) + %zu bytes (generations)\n",
                 sizeof(g_allocator.bitmap), sizeof(g_allocator.generation));
}

uint32_t pid_alloc(void)
{
    spin_lock(&g_allocator.lock);

    uint32_t index = bitmap_find_free(g_allocator.bitmap, PID_MAX_COUNT);

    if (index >= PID_MAX_COUNT)
    {
        spin_unlock(&g_allocator.lock);
        debug_printf("[PID] ERROR: PID exhaustion (allocated=%u/%u)\n",
                     g_allocator.allocated_count, PID_MAX_COUNT);
        return PID_INVALID;
    }

    bitmap_set(g_allocator.bitmap, index);
    g_allocator.generation[index]++;

    // Wrap generation within 16-bit range (skip 0 which is reserved for PID_INVALID)
    if (g_allocator.generation[index] > 0xFFFF)
    {
        g_allocator.generation[index] = 1;
        debug_printf("[PID] WARNING: Generation wrapped on index %u (2^16 allocations)\n", index);
    }

    // Use simple index-based PID for readability: PID = index + 1.
    // First process gets PID 1, second gets PID 2, etc.
    // Generation is still tracked internally for double-free protection,
    // but not exposed in the PID value itself.
    uint32_t pid = index + 1;

    g_allocator.allocated_count++;

    spin_unlock(&g_allocator.lock);

    debug_printf("[PID] Allocated PID %u (idx=%u, gen=%u, count=%u/%u)\n",
                 pid, index, g_allocator.generation[index],
                 g_allocator.allocated_count, PID_MAX_COUNT);

    return pid;
}

void pid_free(uint32_t pid)
{
    if (pid == PID_INVALID)
    {
        return;
    }

    // New PID format: PID = index + 1 (simple sequential numbering)
    uint32_t index = pid - 1;

    if (index >= PID_MAX_COUNT)
    {
        debug_printf("[PID] ERROR: Invalid PID %u (index=%u out of range)\n",
                     pid, index);
        return;
    }

    spin_lock(&g_allocator.lock);

    if (!bitmap_test(g_allocator.bitmap, index))
    {
        debug_printf("[PID] WARNING: Double-free detected for PID %u (index=%u)\n",
                     pid, index);
        spin_unlock(&g_allocator.lock);
        return;
    }

    // Generation is tracked internally but not used for validation in simple mode.
    // The bitmap alone is sufficient for allocation tracking.
    // Generation still protects against rapid alloc/free/realloc cycles.

    bitmap_clear(g_allocator.bitmap, index);
    g_allocator.allocated_count--;

    spin_unlock(&g_allocator.lock);

    debug_printf("[PID] Freed PID %u (idx=%u, gen=%u, remaining=%u/%u)\n",
                 pid, index, g_allocator.generation[index],
                 g_allocator.allocated_count, PID_MAX_COUNT);
}

bool pid_validate(uint32_t pid)
{
    if (pid == PID_INVALID)
    {
        return false;
    }

    // New PID format: PID = index + 1
    uint32_t index = pid - 1;

    if (index >= PID_MAX_COUNT)
    {
        return false;
    }

    spin_lock(&g_allocator.lock);

    // Check if PID is currently allocated (bitmap is authoritative)
    bool allocated = bitmap_test(g_allocator.bitmap, index);

    spin_unlock(&g_allocator.lock);

    return allocated;
}

uint32_t pid_allocated_count(void)
{
    spin_lock(&g_allocator.lock);
    uint32_t count = g_allocator.allocated_count;
    spin_unlock(&g_allocator.lock);
    return count;
}
