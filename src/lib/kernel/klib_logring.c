/* klib_logring.c — the log ring. See klib_logring.h for why it exists.
 *
 * Locking: its own IRQ-safe spinlock, taken last and never held while taking
 * another. kprintf reaches it holding the console lock, hw.log.say the same
 * way, and perf_trace with no lock at all; console -> ring is the whole of the
 * ordering, and no path takes the console lock holding this one.
 *
 * spin_lock() disables interrupts on the acquiring core, so a timer or a
 * controller interrupt that lands mid-append and prints cannot deadlock
 * against the append it interrupted.
 */
#include "klib.h"
#include "klib_logring.h"
#include "boxos_logkeep.h"
#include "serial.h"     /* WireKick — the line is this ring's reader */
#include "vmm.h"
#include "cpu.h"            /* cpu_cpuid — which write-back this part has */
#include "kernel_config.h"  /* CONFIG_CACHE_LINE_SIZE */
#include "baton.h"          /* Knock — the bell is rung from a K-Core */
#include "kring.h"          /* KResultPush — the bell's record */
#include "process.h"        /* process_find_ref */
#include "idt.h"            /* IdtNmiOrMcHere — no knock from an NMI or a #MC */
#include "klib_panic.h"     /* PanicClaimed — nor from a panic */

_Static_assert((LOGRING_CAPACITY & (LOGRING_CAPACITY - 1)) == 0,
               "LOGRING_CAPACITY must be a power of two — the position maps "
               "to a slot by mask");

#define LOGRING_MASK ((uint64_t)LOGRING_CAPACITY - 1u)

static spinlock_t g_ring_lock;
static char       g_ring[LOGRING_CAPACITY];
static uint8_t    g_ring_tone[LOGRING_CAPACITY];   /* the Tone of the byte at the same slot */

/* Counted from the first byte this kernel ever said; never restarts, so a
 * position names the same byte for the life of the boot. */
static uint64_t   g_written;

/* The position just past the end of the last utterance (LogRingSaid). */
static uint64_t   g_said;

/* The bell: the pid that hung it, with LOG_BELL_RUNG once an utterance has
 * marked it, and that strand's generation — a pid is let again, and the bell
 * rings the incarnation that hung it or nobody. Under g_ring_lock. The ring
 * marks it; the knock's visit takes it down as it rings, so one hang rings
 * once; the next hang replaces it, release and teardown drop it. */
#define LOG_BELL_RUNG 0x80000000u
static uint32_t   g_bell;
static uint32_t   g_bell_generation;
static uint32_t   g_bell_due;   /* an utterance marked the bell; nobody has knocked yet */

static void LogRingBellRing(void *ctx);
static Knock g_bell_knock = {
    .baton  = { .next = NULL, .run = LogRingBellRing, .ctx = NULL },
    .raised = 0,
};

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

uint8_t LogRingToneAt(uint64_t pos)
{
    return g_ring_tone[pos & LOGRING_MASK];
}

/* ==========================================================================
 * The carry-over window. See klib_logring.h for what it survives and what it
 * does not.
 * ========================================================================== */

/* The window's place and layout are boxos_logkeep.h's: Gangway's panel reads
 * the bank of the start before by them. A bank holds what the ring does. */
_Static_assert(LOGRING_CAPACITY == LOGKEEP_BANK_BYTES, "a carry-over bank holds the ring");

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
    return keep_base() + LOGKEEP_HEADER + (uint64_t)which * LOGKEEP_BANK_BYTES;
}

/* ── getting the window out of the cache and into the memory ─────────────
 *
 * The window is ordinary write-back RAM, so a byte stored into it lives in
 * this core's cache until something evicts it — and a reset does not evict
 * it. Intel SDM Vol 3A, "Cache Control", Table 9-1 (IA-32 Processor States
 * Following Power-up, Reset, or INIT) gives the cache state after RESET as
 * invalid, and §11.5.1 says the caches are invalidated WITHOUT writeback on
 * a hardware reset. So the tail that matters most — the seconds before the
 * button was pressed — is exactly the part a cached window loses.
 *
 * Written back as the bank is FILLED, one line per line, rather than at a
 * flush point: a machine that wedges never reaches a flush point, and that
 * is the machine this file exists for. LogKeepFlush closes the last partial
 * line for the paths that know they are about to reset or halt.
 *
 * Which instruction is a CPUID question, asked once:
 *   CLWB       CPUID.(EAX=07H,ECX=0):EBX[24] — writes back, keeps the line
 *   CLFLUSHOPT CPUID.(EAX=07H,ECX=0):EBX[23] — writes back and evicts
 *   CLFLUSH    CPUID.01H:EDX[19] (CLFSH)     — the same, strongly ordered
 * CLWB is preferred because the log is appended to continuously and there is
 * no reason to give the line up every 64 bytes. All three are ordered with
 * respect to older stores to the SAME line (SDM Vol 2A, CLFLUSHOPT and CLWB:
 * "ordered with respect to ... older writes to the cache line being written
 * back"), which is the only ordering the fill path needs — each line is
 * complete before it is written back. The two weakly-ordered forms need an
 * SFENCE to be COMPLETE before anything else, and that belongs on the flush
 * path, not on every line. */
typedef enum {
    KeepWbNone = 0,
    KeepWbClflush,
    KeepWbClflushOpt,
    KeepWbClwb
} KeepWriteBack;

static KeepWriteBack g_keep_wb;
static uint64_t      g_keep_line = (uint64_t)CONFIG_CACHE_LINE_SIZE;

static void keep_writeback_pick(void)
{
    uint32_t a = 0, b = 0, c = 0, d = 0;

    /* CPUID.01H:EBX[15:8] is the CLFLUSH line size in eight-byte quantums
     * (SDM Vol 2A, CPUID, Basic CPUID Information). A part that answers 0 —
     * which no x86_64 part does — gets this kernel's declared line size
     * rather than a number invented here. */
    cpu_cpuid(1, 0, &a, &b, &c, &d);
    uint64_t said = (uint64_t)((b >> 8) & 0xFFu) * 8u;
    if (said && (said & (said - 1u)) == 0u) g_keep_line = said;

    bool clflush = (d & (1u << 19)) != 0u;

    uint32_t max_leaf = 0, ub = 0, uc = 0, ud = 0;
    cpu_cpuid(0, 0, &max_leaf, &ub, &uc, &ud);

    bool clflushopt = false, clwb = false;
    if (max_leaf >= 7u) {
        cpu_cpuid(7, 0, &a, &b, &c, &d);
        clflushopt = (b & (1u << 23)) != 0u;
        clwb       = (b & (1u << 24)) != 0u;
    }

    g_keep_wb = clwb       ? KeepWbClwb
              : clflushopt ? KeepWbClflushOpt
              : clflush    ? KeepWbClflush
                           : KeepWbNone;
}

static inline void keep_writeback_line(volatile void *at)
{
    const void *p = (const void *)at;
    switch (g_keep_wb) {
    case KeepWbClwb:
        __asm__ volatile("clwb (%0)"       :: "r"(p) : "memory");
        break;
    case KeepWbClflushOpt:
        __asm__ volatile("clflushopt (%0)" :: "r"(p) : "memory");
        break;
    case KeepWbClflush:
        __asm__ volatile("clflush (%0)"    :: "r"(p) : "memory");
        break;
    case KeepWbNone:
    default:
        break;
    }
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
 * answered it the same way (PhysWrite16 in gangway_memory.c). The memory is real; the
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

static inline void keep_put(char c);

void LogKeepInit(bool keep)
{
    if (!keep || !keep_window_is_ram(LOGKEEP_PHYS, LOGKEEP_BYTES)) {
        g_keep_ok = false;
        g_keep_previous = LOGKEEP_BANKS;
        return;
    }

    g_keep_ok = true;
    keep_writeback_pick();

    volatile LogKeepHeader *h = keep_header();
    bool carried = (h->magic == LOGKEEP_MAGIC) &&
                   (h->size == (uint32_t)sizeof(LogKeepHeader)) &&
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
        h->size     = (uint32_t)sizeof(LogKeepHeader);
        h->capacity = (uint32_t)LOGRING_CAPACITY;
        h->boot     = 1u;
        h->bytes[0] = 0;
        h->bytes[1] = 0;
    }

    h->current               = g_keep_current;
    h->bytes[g_keep_current] = 0;

    /* The header the NEXT boot reads is the one thing that must be in memory
     * before this boot says a word: a window whose magic never left the
     * cache reads as "nothing was carried" after a reset. */
    keep_writeback_line((volatile void *)h);
    __asm__ volatile("sfence" ::: "memory");

    /* What was said before the pass could be read goes in first. One core
     * runs, and nothing else writes the ring yet. */
    uint64_t written = LogRingWritten();
    uint64_t from    = written > (uint64_t)LOGRING_CAPACITY ? written - (uint64_t)LOGRING_CAPACITY : 0;
    for (uint64_t p = from; p < written; p++) keep_put(g_ring[p & LOGRING_MASK]);
}

const char *LogKeepWriteBackName(void)
{
    if (!g_keep_ok) return "none";
    switch (g_keep_wb) {
    case KeepWbClwb:       return "CLWB";
    case KeepWbClflushOpt: return "CLFLUSHOPT";
    case KeepWbClflush:    return "CLFLUSH";
    case KeepWbNone:
    default:               return "WBINVD";
    }
}

/* Everything said so far, in memory rather than in a cache, for a caller that
 * is about to reset or halt the machine — and for the panic path, which is
 * the one caller that cannot come back and ask again.
 *
 * Takes no lock, allocates nothing and waits on nothing, so it is safe with
 * every AP already frozen. Only the LAST line can still be outstanding: every
 * line before it was written back as it was filled. */
void LogKeepFlush(void)
{
    if (!g_keep_ok) return;

    if (g_keep_wb == KeepWbNone) {
        /* No CLFLUSH at all — which no x86_64 part reports, but the fallback
         * that always exists in ring 0 (SDM Vol 2C, WBINVD: writes back all
         * modified cache lines and invalidates the caches). */
        __asm__ volatile("wbinvd" ::: "memory");
        return;
    }

    volatile LogKeepHeader *h = keep_header();
    uint64_t n = h->bytes[g_keep_current];
    if (n) {
        volatile uint8_t *bank = keep_bank(g_keep_current);
        keep_writeback_line(&bank[(n - 1u) & LOGRING_MASK]);
    }
    keep_writeback_line((volatile void *)h);

    /* CLFLUSHOPT and CLWB are weakly ordered; SDM Vol 2A says a store fence
     * is what makes them complete with respect to what follows. CLFLUSH is
     * fenced the same way rather than by MFENCE because SFENCE orders it
     * too and this path must not be told apart by instruction. */
    __asm__ volatile("sfence" ::: "memory");
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

/* The same byte, into the window the next boot will find. The counter goes
 * down with every byte rather than at some flush point — a machine that
 * wedges never reaches a flush point, and that is the machine this exists
 * for. The count and the bytes it names leave the cache TOGETHER, so a
 * window read after a reset never names a byte that is not in it. */
static inline void keep_put(char c)
{
    if (!g_keep_ok) return;
    volatile LogKeepHeader *h = keep_header();
    uint64_t n = h->bytes[g_keep_current];
    volatile uint8_t *slot = &keep_bank(g_keep_current)[n & LOGRING_MASK];
    *slot = (uint8_t)c;
    h->bytes[g_keep_current] = n + 1;

    /* The byte that completed a cache line takes that line back to memory,
     * and the header with it, so the count never names bytes that are still
     * in a cache. One pair of write-backs per LINE, not per byte. The bank
     * starts at a 4 KiB boundary of the window, so the slot's low bits are
     * its true offset within the line. */
    if (((uintptr_t)slot & (g_keep_line - 1u)) == g_keep_line - 1u) {
        keep_writeback_line(slot);
        keep_writeback_line((volatile void *)h);
    }
}



/* One byte in, under the lock. The tone goes in before the store that
 * publishes the position, so a lock-free reader never sees a byte without
 * its tone. */
static inline void ring_put_locked(char c, uint8_t tone)
{
    g_ring[g_written & LOGRING_MASK]      = c;
    g_ring_tone[g_written & LOGRING_MASK] = tone;
    __atomic_store_n(&g_written, g_written + 1u, __ATOMIC_RELEASE);
    keep_put(c);
}

static inline void ring_said_locked(void)
{
    g_said = g_written;
    if (g_bell && !(g_bell & LOG_BELL_RUNG)) {
        g_bell |= LOG_BELL_RUNG;
        __atomic_store_n(&g_bell_due, 1u, __ATOMIC_RELEASE);
    }
}

void LogRingPut(char c, uint8_t tone)
{
    spin_lock(&g_ring_lock);
    ring_put_locked(c, tone);
    spin_unlock(&g_ring_lock);

    /* The lock is down: the wire takes its own, and never this one. */
    WireKick();
}

void LogRingSay(const char *bytes, uint64_t n, uint8_t tone,
                uint64_t *out_start, uint64_t *out_end)
{
    spin_lock(&g_ring_lock);
    uint64_t start = g_written;
    for (uint64_t i = 0; i < n; i++)
        ring_put_locked(bytes[i], tone);
    ring_said_locked();
    uint64_t end = g_written;
    spin_unlock(&g_ring_lock);

    WireKick();
    if (out_start) *out_start = start;
    if (out_end)   *out_end   = end;
}

void LogRingSaid(void)
{
    spin_lock(&g_ring_lock);
    ring_said_locked();
    spin_unlock(&g_ring_lock);
}

bool LogRingBellHang(uint32_t pid, uint32_t generation, uint64_t from)
{
    spin_lock(&g_ring_lock);
    bool news = g_said > from;
    if (!news) {
        g_bell            = pid;
        g_bell_generation = generation;
    }
    spin_unlock(&g_ring_lock);
    return news;
}

void LogRingBellDrop(void)
{
    spin_lock(&g_ring_lock);
    g_bell = 0;
    spin_unlock(&g_ring_lock);
}

void LogRingBellKnock(void)
{
    if (!__atomic_load_n(&g_bell_due, __ATOMIC_ACQUIRE)) return;
    if (IdtNmiOrMcHere()) return;   /* the BSP's tick knocks for them */
    if (PanicClaimed())   return;   /* every core stands: nobody to wake */
    if (!__atomic_exchange_n(&g_bell_due, 0u, __ATOMIC_ACQ_REL)) return;
    KnockOn(&g_bell_knock);
}

/* On a K-Core, every lock down. The bell is read again rather than carried:
 * a holder that woke for something else, read the ring and hung a fresh bell
 * in the meantime has nothing new to be woken for, and finds its bell
 * unmarked here. */
static void LogRingBellRing(void *ctx)
{
    (void)ctx;
    KnockOpen(&g_bell_knock);

    spin_lock(&g_ring_lock);
    uint32_t bell       = g_bell;
    uint32_t generation = g_bell_generation;
    if (bell & LOG_BELL_RUNG) g_bell = 0;   /* rung: taken down, rung once */
    spin_unlock(&g_ring_lock);
    if (!(bell & LOG_BELL_RUNG)) return;

    process_t *holder = process_find_ref(bell & ~LOG_BELL_RUNG);
    if (!holder) return;
    if (holder->generation == generation) {
        Result r;
        memset(&r, 0, sizeof(r));
        r.error_code = ERR_WOULD_BLOCK;
        (void)KResultPush(holder, &r);
    }
    process_ref_dec(holder);
}

static uint64_t ring_read(uint64_t from, void *dst, uint8_t *tones, uint64_t max,
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
        if (tones) {
            memcpy(tones, &g_ring_tone[off], (size_t)first);
            if (copied > first)
                memcpy(tones + first, &g_ring_tone[0], (size_t)(copied - first));
        }
    }

    spin_unlock(&g_ring_lock);

    if (out_oldest)  *out_oldest  = oldest;
    if (out_written) *out_written = written;
    return copied;
}

uint64_t LogRingRead(uint64_t from, void *dst, uint64_t max,
                     uint64_t *out_oldest, uint64_t *out_written)
{
    return ring_read(from, dst, NULL, max, out_oldest, out_written);
}

uint64_t LogRingReadTones(uint64_t from, void *dst, uint8_t *tones, uint64_t max,
                          uint64_t *out_oldest, uint64_t *out_written)
{
    return ring_read(from, dst, tones, max, out_oldest, out_written);
}
