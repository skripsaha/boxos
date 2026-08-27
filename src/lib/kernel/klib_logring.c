/* klib_logring.c — the log ring. See klib_logring.h for why it exists.
 *
 * Locking: its own IRQ-safe spinlock, never the console lock. kputchar() is
 * reached two ways — from kprintf, which already holds the console lock, and
 * from the keyboard echo, which does not — so the ring cannot borrow that lock
 * without either missing what was typed or nesting into a lock the caller may
 * already hold. One lock of its own, taken last and never held while taking
 * another, is the whole of the ordering: console -> ring on one path, ring
 * alone on the other two, and no third path exists.
 *
 * spin_lock() disables interrupts on the acquiring core, so a timer or a
 * controller interrupt that lands mid-append and prints cannot deadlock
 * against the append it interrupted.
 */
#include "klib.h"
#include "klib_logring.h"

#ifndef CONFIG_PRINTTOFILE

bool LogRingIsKept(void) { return false; }

#else

_Static_assert((LOGRING_CAPACITY & (LOGRING_CAPACITY - 1)) == 0,
               "LOGRING_CAPACITY must be a power of two — the position maps "
               "to a slot by mask");

#define LOGRING_MASK ((uint64_t)LOGRING_CAPACITY - 1u)

static spinlock_t g_ring_lock;
static char       g_ring[LOGRING_CAPACITY];

/* Counted from the first byte this kernel ever said; never restarts, so a
 * position names the same byte for the life of the boot. */
static uint64_t   g_written;

bool LogRingIsKept(void) { return true; }

void LogRingLockInit(void)
{
    spinlock_init(&g_ring_lock);
}

void LogRingPut(char c)
{
    spin_lock(&g_ring_lock);
    g_ring[g_written & LOGRING_MASK] = c;
    g_written++;
    spin_unlock(&g_ring_lock);
}

uint64_t LogRingRead(uint64_t from, void *dst, uint64_t max,
                     uint64_t *out_oldest, uint64_t *out_written)
{
    char    *out    = (char *)dst;
    uint64_t copied = 0;
    uint64_t written, oldest;

    spin_lock(&g_ring_lock);

    written = g_written;
    oldest  = (written > (uint64_t)LOGRING_CAPACITY)
            ? written - (uint64_t)LOGRING_CAPACITY : 0;

    /* A reader the writers have overtaken is moved forward to the oldest byte
     * still held rather than handed whatever now sits at its old position.
     * `oldest` goes back with the answer, so the caller can say how much it
     * missed instead of printing a seam it cannot see. */
    uint64_t start = (from < oldest) ? oldest : from;

    if (out && max && start < written) {
        uint64_t avail = written - start;
        copied = (avail < max) ? avail : max;

        /* Two spans at most: to the end of the buffer, then from its front.
         * Copied with memcpy rather than byte by byte because this runs with
         * interrupts off, and the length of that window is the only cost the
         * rest of the machine pays for being asked what it said. */
        uint64_t off   = start & LOGRING_MASK;
        uint64_t first = (uint64_t)LOGRING_CAPACITY - off;
        if (first > copied) first = copied;

        memcpy(out, &g_ring[off], (size_t)first);
        if (copied > first) {
            memcpy(out + first, &g_ring[0], (size_t)(copied - first));
        }
    }

    spin_unlock(&g_ring_lock);

    if (out_oldest)  *out_oldest  = oldest;
    if (out_written) *out_written = written;
    return copied;
}

#endif /* CONFIG_PRINTTOFILE */
