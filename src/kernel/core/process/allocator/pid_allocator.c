#include "pid_allocator.h"
#include "klib.h"
#include "io.h"

#define PID_BITMAP_QWORDS ((PID_MAX_COUNT + 63) / 64)
_Static_assert((PID_MAX_COUNT % 64) == 0,
               "PID_MAX_COUNT must be a multiple of 64 for the qword-bitmap scan");

typedef struct
{
    uint64_t bitmap[PID_BITMAP_QWORDS];
    uint32_t generation[PID_MAX_COUNT];
    uint32_t allocated_count;
    spinlock_t lock;
} pid_allocator_t;

static pid_allocator_t g_allocator;

static inline void bitmap_set(uint64_t *bitmap, uint32_t index)
{
    bitmap[index >> 6] |= (1ULL << (index & 63));
}

static inline void bitmap_clear(uint64_t *bitmap, uint32_t index)
{
    bitmap[index >> 6] &= ~(1ULL << (index & 63));
}

static inline bool bitmap_test(const uint64_t *bitmap, uint32_t index)
{
    return (bitmap[index >> 6] & (1ULL << (index & 63))) != 0;
}

static uint32_t bitmap_find_free(const uint64_t *bitmap, uint32_t max_bits)
{
    uint32_t qwords = (max_bits + 63) >> 6;
    for (uint32_t w = 0; w < qwords; w++)
    {
        uint64_t v = bitmap[w];
        if (v != ~(uint64_t)0)
        {
            uint32_t index = (w << 6) + (uint32_t)__builtin_ctzll(~v);
            if (index < max_bits)
                return index;
            return max_bits;
        }
    }
    return max_bits;
}

void pid_allocator_init(void)
{
    memset(&g_allocator, 0, sizeof(pid_allocator_t));


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

    if (g_allocator.generation[index] > 0xFFFF)
    {
        g_allocator.generation[index] = 1;
        debug_printf("[PID] WARNING: Generation wrapped on index %u (2^16 allocations)\n", index);
    }

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

    uint32_t index = pid - 1;

    if (index >= PID_MAX_COUNT)
    {
        return false;
    }

    spin_lock(&g_allocator.lock);

    bool allocated = bitmap_test(g_allocator.bitmap, index);

    spin_unlock(&g_allocator.lock);

    return allocated;
}

uint32_t pid_generation(uint32_t pid)
{
    if (pid == PID_INVALID)
    {
        return 0;
    }

    uint32_t index = pid - 1;

    if (index >= PID_MAX_COUNT)
    {
        return 0;
    }

    spin_lock(&g_allocator.lock);

    uint32_t generation = g_allocator.generation[index];

    spin_unlock(&g_allocator.lock);

    return generation;
}

uint32_t pid_allocated_count(void)
{
    spin_lock(&g_allocator.lock);
    uint32_t count = g_allocator.allocated_count;
    spin_unlock(&g_allocator.lock);
    return count;
}

PidLife pid_life(uint32_t pid, uint32_t generation)
{
    if (pid == PID_INVALID || generation == 0) return PID_LIFE_NEVER;

    uint32_t index = pid - 1;
    if (index >= PID_MAX_COUNT) return PID_LIFE_NEVER;

    spin_lock(&g_allocator.lock);
    uint32_t slot_gen  = g_allocator.generation[index];
    bool     allocated = bitmap_test(g_allocator.bitmap, index);
    spin_unlock(&g_allocator.lock);

    if (generation > slot_gen) return PID_LIFE_NEVER;

    if (generation < slot_gen) return PID_LIFE_DEPARTED;

    return allocated ? PID_LIFE_LIVE : PID_LIFE_DEPARTED;
}