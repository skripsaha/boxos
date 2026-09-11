#include "klib.h"
#include "klib_logring.h"
#include "serial.h"
#include "vmm.h"

_Static_assert((LOGRING_CAPACITY & (LOGRING_CAPACITY - 1)) == 0,
               "LOGRING_CAPACITY must be a power of two — the position maps "
               "to a slot by mask");

#define LOGRING_MASK ((uint64_t)LOGRING_CAPACITY - 1u)

static spinlock_t g_ring_lock;
static char       g_ring[LOGRING_CAPACITY];

static uint64_t   g_written;

void LogRingLockInit(void)
{
    spinlock_init(&g_ring_lock);
}

void LogRingForceRelease(void)
{
    spin_force_release(&g_ring_lock);
}

uint64_t LogRingWritten(void)
{
    return __atomic_load_n(&g_written, __ATOMIC_ACQUIRE);
}

char LogRingByteAt(uint64_t pos)
{
    return g_ring[pos & LOGRING_MASK];
}

#ifdef CONFIG_PRINTTOFILE


#define LOGKEEP_PHYS      0x04000000ULL
#define LOGKEEP_HEADER    4096ULL
#define LOGKEEP_BANKS     2u
#define LOGKEEP_BYTES     (LOGKEEP_HEADER + (uint64_t)LOGKEEP_BANKS * LOGRING_CAPACITY)

#define LOGKEEP_MAGIC     0x4449415353584F42ULL
#define LOGKEEP_VERSION   1u

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t capacity;
    uint32_t current;
    uint32_t boot;
    uint64_t bytes[LOGKEEP_BANKS];
} LogKeepHeader;

_Static_assert(sizeof(LogKeepHeader) <= LOGKEEP_HEADER,
               "the carry-over header must fit its page");

static bool g_keep_ok;
static uint32_t          g_keep_current;
static uint32_t          g_keep_previous;
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

static __attribute__((noinline)) void phys_read(uint64_t at, void *dst, unsigned n)
{
    const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)at;
    uint8_t *out = (uint8_t *)dst;
    for (unsigned i = 0; i < n; i++) out[i] = src[i];
}

static bool keep_window_is_ram(uint64_t phys, uint64_t bytes)
{
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

        if (type != 1u) continue;
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

static inline void keep_put(char c)
{
    if (!g_keep_ok) return;
    volatile LogKeepHeader *h = keep_header();
    uint64_t n = h->bytes[g_keep_current];
    keep_bank(g_keep_current)[n & LOGRING_MASK] = (uint8_t)c;
    h->bytes[g_keep_current] = n + 1;
}

#else

static inline void keep_put(char c) { (void)c; }

#endif

void LogRingPut(char c)
{
    spin_lock(&g_ring_lock);
    g_ring[g_written & LOGRING_MASK] = c;
    __atomic_store_n(&g_written, g_written + 1u, __ATOMIC_RELEASE);
    keep_put(c);
    spin_unlock(&g_ring_lock);

    WireKick();
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

    uint64_t start = (from < oldest) ? oldest : from;

    if (out && max && start < written) {
        uint64_t avail = written - start;
        copied = (avail < max) ? avail : max;

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