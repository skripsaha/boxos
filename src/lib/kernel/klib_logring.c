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
#include "vmm.h"

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

/* ==========================================================================
 * The carry-over window. See klib_logring.h for what it survives and what it
 * does not.
 * ========================================================================== */

/* 64 MiB. Above anything either loader places (the kernel may be 32 MiB and
 * its page tables and boot stack sit just past it, ~33 MiB at the very most),
 * below the first address any machine BoxOS targets fails to have. Fixed
 * rather than allocated because the whole point is that the NEXT boot finds
 * it, and the next boot's allocator has not run yet. */
#define LOGKEEP_PHYS      0x04000000ULL
#define LOGKEEP_HEADER    4096ULL
#define LOGKEEP_BANKS     2u
#define LOGKEEP_BYTES     (LOGKEEP_HEADER + (uint64_t)LOGKEEP_BANKS * LOGRING_CAPACITY)

#define LOGKEEP_MAGIC     0x4449415353584F42ULL   /* "BOXSAID" + NUL, little-end */
#define LOGKEEP_VERSION   1u

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t capacity;     /* per bank */
    uint32_t current;      /* bank this boot writes */
    uint32_t boot;         /* counts up; names which run a bank came from */
    uint64_t bytes[LOGKEEP_BANKS];
} LogKeepHeader;

_Static_assert(sizeof(LogKeepHeader) <= LOGKEEP_HEADER,
               "the carry-over header must fit its page");

/* False until LogKeepInit accepts the window. Every use is guarded on it, so a
 * machine that cannot offer the memory simply carries nothing forward.
 *
 * ‼ The ADDRESS is deliberately not cached. It was, once, and it hung the boot
 * on the first try: the pointer was set to the identity address at the top of
 * kernel_main and re-pointed at the Pull Map beside e820_activate_pull_map,
 * and every kprintf issued in between — inside vmm_init, while the identity
 * map is being taken down — wrote through an address that had just stopped
 * existing. vmm_phys_to_virt answers whichever mapping is live at the moment
 * it is asked, which is the only form of this that cannot go stale. */
static bool g_keep_ok;
static uint32_t          g_keep_current;
static uint32_t          g_keep_previous;      /* LOGKEEP_BANKS = none */
static uint64_t          g_keep_prev_bytes;
static uint32_t          g_keep_prev_boot;

static inline volatile uint8_t *keep_base(void)
{
    return (volatile uint8_t *)vmm_phys_to_virt((uintptr_t)LOGKEEP_PHYS);
}

static inline volatile LogKeepHeader *keep_header(void)
{
    return (volatile LogKeepHeader *)keep_base();
}

static inline volatile uint8_t *keep_bank(uint32_t which)
{
    return keep_base() + LOGKEEP_HEADER + (uint64_t)which * LOGRING_CAPACITY;
}

/* Is [phys, phys+bytes) inside one usable E820 entry?
 *
 * Asked of the map at its fixed low address rather than of the kernel's own
 * parsed copy, because this runs before that copy exists — and it has to run
 * that early, or the first lines the kernel says are lost to the next boot.
 * A blind store into 64 MiB on a machine that has less would not be a bug
 * report, it would be a store into whatever answers there. */
/* Read n bytes out of a raw physical address.
 *
 * noinline and through a void* on purpose: GCC traces a cast-from-integer
 * pointer back to its origin, decides the object it points at has zero extent,
 * and fires -Warray-bounds on every access. The loaders hit the same wall and
 * answered it the same way (PhysWrite16 in tagboot.c). The memory is real; the
 * compiler simply has no declaration for it. */
static __attribute__((noinline)) void phys_read(uint64_t at, void *dst, unsigned n)
{
    const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)at;
    uint8_t *out = (uint8_t *)dst;
    for (unsigned i = 0; i < n; i++) out[i] = src[i];
}

static bool keep_window_is_ram(uint64_t phys, uint64_t bytes)
{
    /* The same two addresses both loaders write; see e820.h and stage2.asm. */
    uint16_t count = 0;
    phys_read(0x500, &count, sizeof(count));
    if (count == 0 || count > 128) return false;

    for (uint16_t i = 0; i < count; i++) {
        uint64_t at = 0x504 + (uint64_t)i * 24u;
        uint64_t base = 0, len = 0;
        uint32_t type = 0;
        phys_read(at,      &base, sizeof(base));
        phys_read(at + 8,  &len,  sizeof(len));
        phys_read(at + 16, &type, sizeof(type));

        if (type != 1u) continue;                       /* E820_USABLE */
        if (phys >= base && phys + bytes <= base + len) return true;
    }
    return false;
}

void LogKeepInit(void)
{
    if (!keep_window_is_ram(LOGKEEP_PHYS, LOGKEEP_BYTES)) {
        g_keep_ok = false;
        g_keep_previous = LOGKEEP_BANKS;
        return;
    }

    g_keep_ok = true;

    volatile LogKeepHeader *h = keep_header();
    bool carried = (h->magic == LOGKEEP_MAGIC) &&
                   (h->version == LOGKEEP_VERSION) &&
                   (h->capacity == (uint32_t)LOGRING_CAPACITY) &&
                   (h->current < LOGKEEP_BANKS);

    if (carried) {
        g_keep_previous  = h->current;
        g_keep_prev_bytes = h->bytes[g_keep_previous];
        g_keep_prev_boot  = h->boot;
        g_keep_current   = 1u - g_keep_previous;
        h->boot          = h->boot + 1u;
    } else {
        /* Nothing survived — a first start, a cold one, or firmware that
         * scrubbed the memory. Take the window fresh and say nothing was
         * carried; the boot line in kernel_main reports it. */
        g_keep_previous   = LOGKEEP_BANKS;
        g_keep_prev_bytes = 0;
        g_keep_prev_boot  = 0;
        g_keep_current    = 0;
        h->magic    = LOGKEEP_MAGIC;
        h->version  = LOGKEEP_VERSION;
        h->capacity = (uint32_t)LOGRING_CAPACITY;
        h->boot     = 1u;
        h->bytes[0] = 0;
        h->bytes[1] = 0;
    }

    h->current               = g_keep_current;
    h->bytes[g_keep_current] = 0;
}

bool LogKeepWindow(uintptr_t *out_phys, uint64_t *out_bytes)
{
    if (!g_keep_ok) return false;
    if (out_phys)  *out_phys  = (uintptr_t)LOGKEEP_PHYS;
    if (out_bytes) *out_bytes = LOGKEEP_BYTES;
    return true;
}

uint64_t LogKeepPreviousBytes(void)
{
    return (g_keep_ok && g_keep_previous < LOGKEEP_BANKS) ? g_keep_prev_bytes : 0;
}

uint32_t LogKeepPreviousBoot(void)
{
    return (g_keep_ok && g_keep_previous < LOGKEEP_BANKS) ? g_keep_prev_boot : 0;
}

uint64_t LogKeepPreviousRead(uint64_t from, void *dst, uint64_t max,
                             uint64_t *out_oldest, uint64_t *out_written)
{
    uint64_t written = LogKeepPreviousBytes();
    uint64_t oldest  = (written > (uint64_t)LOGRING_CAPACITY)
                     ? written - (uint64_t)LOGRING_CAPACITY : 0;
    uint64_t copied  = 0;

    /* No lock: the previous boot's bank has exactly one writer — a boot that
     * has already ended — and this boot never touches it. */
    if (g_keep_ok && g_keep_previous < LOGKEEP_BANKS && dst && max) {
        uint64_t start = (from < oldest) ? oldest : from;
        if (start < written) {
            volatile uint8_t *bank = keep_bank(g_keep_previous);
            uint64_t avail = written - start;
            copied = (avail < max) ? avail : max;
            uint8_t *out = (uint8_t *)dst;
            for (uint64_t i = 0; i < copied; i++) {
                out[i] = bank[(start + i) & LOGRING_MASK];
            }
        }
    }

    if (out_oldest)  *out_oldest  = oldest;
    if (out_written) *out_written = written;
    return copied;
}

void LogRingPut(char c)
{
    spin_lock(&g_ring_lock);
    g_ring[g_written & LOGRING_MASK] = c;
    g_written++;

    /* The same byte, into the window the next boot will find. Two stores
     * instead of one, both into RAM, and the counter goes down with every byte
     * rather than at some flush point — a machine that wedges never reaches a
     * flush point, and that is the machine this exists for. */
    if (g_keep_ok) {
        volatile LogKeepHeader *h = keep_header();
        uint64_t n = h->bytes[g_keep_current];
        keep_bank(g_keep_current)[n & LOGRING_MASK] = (uint8_t)c;
        h->bytes[g_keep_current] = n + 1;
    }

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
