
#include "box/brook.h"
#include "box/types.h"
#include "box/string.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/clock.h"
#include "box/system.h"
#include "box/cpu.h"
#include "box/core/manifest.h"
#include "box/core/pocket.h"
#include "box/turnin.h"
#include "box/core/strand_self.h"
#include "boxos_decks.h"

#define BROOK_FRAME_SIZE_MIN     8u
#define BROOK_FRAME_SIZE_MAX     (64u * 1024u)
#define BROOK_FRAME_COUNT_MIN    2u
#define BROOK_FRAME_COUNT_MAX    16384u
#define BROOK_MAX_TOTAL_SIZE     (1ULL * 1024 * 1024 * 1024)

#define BROOK_ALIVE_FROZEN       0xFFFFFFFFu

#define BROOK_SPIN_BUDGET        2048u

#define BROOK_BELL_RUNG   0x80000000u

typedef struct {
    volatile uint64_t tail;
    volatile uint32_t writer_alive;
    volatile uint32_t writer_ever_attached;
    uint32_t          frame_size;
    uint32_t          frame_count;
    uint64_t          magic;
    volatile uint32_t writer_bell;
    uint8_t           _pad_line0[28];

    volatile uint64_t head;
    volatile uint32_t reader_alive;
    volatile uint32_t reader_ever_attached;
    volatile uint32_t reader_bell;
    uint8_t           _pad_line1[44];
} BrookHeaderUser;

STATIC_ASSERT(sizeof(BrookHeaderUser) == 128,
              "BrookHeaderUser must match kernel BrookHeader (128 B)");
STATIC_ASSERT(OFFSETOF(BrookHeaderUser, tail) == 0,
              "BrookHeaderUser.tail offset must be 0 (CL0)");
STATIC_ASSERT(OFFSETOF(BrookHeaderUser, head) == 64,
              "BrookHeaderUser.head offset must be 64 (CL1)");
STATIC_ASSERT(OFFSETOF(BrookHeaderUser, writer_alive) < 64,
              "writer_alive must share CL0 with tail");
STATIC_ASSERT(OFFSETOF(BrookHeaderUser, reader_alive) >= 64,
              "reader_alive must share CL1 with head");
STATIC_ASSERT(OFFSETOF(BrookHeaderUser, reader_bell) >= 64,
              "reader_bell must share CL1 with head — the line the writer already reads");
STATIC_ASSERT(OFFSETOF(BrookHeaderUser, writer_bell) < 64,
              "writer_bell must share CL0 with tail — the line the reader already reads");

struct Brook {
    BrookHeaderUser *hdr;
    uint8_t         *slots;
    uint64_t         va_header;
    uint32_t         frame_size;
    uint32_t         frame_count;
    uint32_t         role;
    uint32_t         streaming;
    uint32_t         self_pid;
};

static inline uint64_t brook_load_acquire_u64(volatile uint64_t *p)
{ return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline uint64_t brook_load_relaxed_u64(volatile uint64_t *p)
{ return __atomic_load_n(p, __ATOMIC_RELAXED); }
static inline void brook_store_release_u64(volatile uint64_t *p, uint64_t v)
{ __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline uint32_t brook_load_acquire_u32(volatile uint32_t *p)
{ return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

static inline void brook_cpu_pause(void)
{
    __asm__ volatile("pause" ::: "memory");
}

static inline bool brook_cas_u32(volatile uint32_t *p, uint32_t expect, uint32_t want)
{
    return __atomic_compare_exchange_n(p, &expect, want, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

static void brook_ring(volatile uint32_t *bell)
{
    uint32_t who = brook_load_acquire_u32(bell);
    if (who == 0) return;
    if (who & BROOK_BELL_RUNG) return;
    if (!brook_cas_u32(bell, who, who | BROOK_BELL_RUNG)) return;

    uint8_t         mbuf[96];
    ManifestBuilder mb;
    uint8_t         params[4];
    int             rc = -1;

    memcpy(params, &who, sizeof(uint32_t));
    if (ManifestBuilderInit(&mb, mbuf, sizeof(mbuf)) == 0 &&
        ManifestBuilderAddOp(&mb, DECK_SYSTEM, SYSTEM_OP_BELL, 0,
                             CRATE_INDEX_NONE, CRATE_INDEX_NONE,
                             params, sizeof(params)) == 0 &&
        ManifestBuilderFinalize(&mb) == 0) {
        rc = ManifestSubmitNoWait((const Manifest *)mbuf, NULL, 0, 0);
    }

    if (rc != 0) {
        (void)brook_cas_u32(bell, who | BROOK_BELL_RUNG, who);
    }
}

static inline volatile uint32_t *brook_own_bell(BrookHeaderUser *h, uint32_t role)
{
    return (role == BROOK_WRITER) ? &h->writer_bell : &h->reader_bell;
}

static inline volatile uint32_t *brook_peer_bell(BrookHeaderUser *h, uint32_t role)
{
    return (role == BROOK_WRITER) ? &h->reader_bell : &h->writer_bell;
}

static bool brook_pop_ready(Brook *b)
{
    BrookHeaderUser *h = b->hdr;
    uint64_t head = brook_load_relaxed_u64(&h->head);
    uint64_t tail = brook_load_acquire_u64(&h->tail);
    if (head != tail) return true;
    if (b->streaming) return false;
    return brook_load_acquire_u32(&h->writer_alive) == 0 &&
           brook_load_acquire_u32(&h->writer_ever_attached) != 0;
}

static bool brook_push_ready(Brook *b)
{
    BrookHeaderUser *h = b->hdr;
    uint64_t tail = brook_load_relaxed_u64(&h->tail);
    uint64_t head = brook_load_acquire_u64(&h->head);
    if ((tail - head) < b->frame_count) return true;
    if (b->streaming) return false;
    return brook_load_acquire_u32(&h->reader_alive) == 0 &&
           brook_load_acquire_u32(&h->reader_ever_attached) != 0;
}

static void brook_sleep_until(Brook *b, bool (*recheck)(Brook *))
{
    BrookHeaderUser   *h    = b->hdr;
    volatile uint32_t *mine = brook_own_bell(h, b->role);

    __atomic_store_n(mine, b->self_pid, __ATOMIC_SEQ_CST);

    TurnInMark mark = box_mark();
    if (!recheck(b)) box_turn_in(mark);

    __atomic_store_n(mine, 0u, __ATOMIC_RELEASE);
}

static int brook_sys_open(const char *tag,
                          uint32_t frame_size, uint32_t frame_count,
                          uint32_t flags,
                          uint64_t *out_va_header,
                          uint64_t *out_va_slots,
                          uint32_t *out_frame_size,
                          uint32_t *out_frame_count)
{
    uint8_t params[12];
    memcpy(params,     &frame_size,  sizeof(uint32_t));
    memcpy(params + 4, &frame_count, sizeof(uint32_t));
    memcpy(params + 8, &flags,       sizeof(uint32_t));

    uint8_t out[24] = {0};
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_BROOK_OPEN,
                     params, sizeof(params),
                     tag, (uint32_t)(strlen(tag) + 1),
                     out, sizeof(out), 0,
                     0 , 0);
    if (rc != 0) return rc;
    memcpy(out_va_header,    out,      sizeof(uint64_t));
    memcpy(out_va_slots,     out + 8,  sizeof(uint64_t));
    memcpy(out_frame_size,   out + 16, sizeof(uint32_t));
    memcpy(out_frame_count,  out + 20, sizeof(uint32_t));
    return 0;
}

static int brook_sys_release(uint64_t va_header)
{
    uint8_t params[8];
    memcpy(params, &va_header, sizeof(uint64_t));
    return MfCall1(DECK_SYSTEM, SYSTEM_OP_BROOK_RELEASE,
                   params, sizeof(params), 0, 0, 0, 0, 0, 0 , 0);
}

static bool brook_is_pow2_u32(uint32_t v)
{ return v != 0 && (v & (v - 1)) == 0; }

static int brook_validate_create_shape(uint32_t fs, uint32_t fc)
{
    if (fs < BROOK_FRAME_SIZE_MIN  || fs > BROOK_FRAME_SIZE_MAX)  return -ERR_INVALID_ARGS;
    if (fc < BROOK_FRAME_COUNT_MIN || fc > BROOK_FRAME_COUNT_MAX) return -ERR_INVALID_ARGS;
    if (!brook_is_pow2_u32(fc))                                   return -ERR_INVALID_ARGS;
    if ((uint64_t)fs * fc > BROOK_MAX_TOTAL_SIZE)                 return -ERR_INVALID_ARGS;
    return 0;
}

Brook *brook_open(const char *tag, uint32_t frame_size, uint32_t frame_count,
                  uint32_t flags)
{
    if (!tag || tag[0] == '\0') return 0;

    uint32_t role = flags & (BROOK_WRITER | BROOK_READER);
    if (role != BROOK_WRITER && role != BROOK_READER) return 0;

    if (flags & BROOK_CREATE) {
        if (brook_validate_create_shape(frame_size, frame_count) != 0) return 0;
    }

    uint64_t va_header = 0, va_slots = 0;
    uint32_t out_fs = 0, out_fc = 0;
    int rc = brook_sys_open(tag, frame_size, frame_count, flags,
                            &va_header, &va_slots, &out_fs, &out_fc);
    if (rc != 0) return 0;

    Brook *b = (Brook *)malloc(sizeof(Brook));
    if (!b) {
        brook_sys_release(va_header);
        return 0;
    }
    memset(b, 0, sizeof(*b));
    b->hdr         = (BrookHeaderUser *)(uintptr_t)va_header;
    b->slots       = (uint8_t *)(uintptr_t)va_slots;
    b->va_header   = va_header;
    b->frame_size  = out_fs;
    b->frame_count = out_fc;
    b->role        = role;
    b->streaming   = (flags & BROOK_STREAM) ? 1u : 0u;
    b->self_pid    = strand_self();
    return b;
}

static inline bool brook_cas_freeze_peer(volatile uint32_t *peer_alive)
{
    uint32_t expected = 0u;
    if (__atomic_compare_exchange_n(peer_alive, &expected,
                                    BROOK_ALIVE_FROZEN,
                                    false,
                                    __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        return true;
    }
    return expected == BROOK_ALIVE_FROZEN;
}

int brook_release(Brook *b)
{
    if (!b) return -ERR_INVALID_ARGS;
    if (b->hdr)
        __atomic_store_n(brook_own_bell(b->hdr, b->role), 0u, __ATOMIC_RELEASE);
    int rc = brook_sys_release(b->va_header);
    free(b);
    return rc;
}

#define BROOK_UMWAIT_DEADLINE_CAP_MS    50u

static void brook_wait_cycle(volatile void *watch_addr,
                             uint32_t *spin_counter,
                             uint64_t deadline_ms)
{
    if (cpu_has_waitpkg()) {
        umonitor(watch_addr);

        uint64_t budget_ms = BROOK_UMWAIT_DEADLINE_CAP_MS;
        if (deadline_ms != 0) {
            uint64_t now_ms = clock_uptime_ms();
            if (now_ms >= deadline_ms) return;
            uint64_t remaining = deadline_ms - now_ms;
            if (remaining < budget_ms) budget_ms = remaining;
        }
        uint64_t deadline_tsc = cpu_rdtsc() + cpu_ms_to_tsc(budget_ms);

        (void)umwait(0, deadline_tsc);
        return;
    }

    if ((*spin_counter)++ < BROOK_SPIN_BUDGET) {
        brook_cpu_pause();
    } else {
        *spin_counter = 0;
        yield();
    }
}

typedef enum {
    BROOK_MODE_TRY     = 0,
    BROOK_MODE_BLOCK   = 1,
    BROOK_MODE_TIMEOUT = 2,
} BrookWaitMode;

static int brook_push_core(Brook *b, const void *frame,
                           BrookWaitMode mode, uint64_t deadline_ms)
{
    if (!b || !frame)             return -ERR_INVALID_ARGS;
    if (b->role != BROOK_WRITER)  return -ERR_INVALID_ARGS;

    BrookHeaderUser *h = b->hdr;
    uint32_t cap = b->frame_count;
    uint32_t cap_mask = cap - 1;
    uint32_t fs = b->frame_size;

    uint32_t spin = 0;
    for (;;) {
        uint64_t tail = brook_load_relaxed_u64(&h->tail);
        uint64_t head = brook_load_acquire_u64(&h->head);

        if ((tail - head) < cap) {
            uint8_t *slot = b->slots + ((uint32_t)(tail & cap_mask)) * fs;
            memcpy(slot, frame, fs);

            __atomic_exchange_n(&h->tail, tail + 1, __ATOMIC_SEQ_CST);

            brook_ring(&h->reader_bell);
            return 0;
        }

        if (!b->streaming &&
            brook_load_acquire_u32(&h->reader_alive) == 0 &&
            brook_load_acquire_u32(&h->reader_ever_attached) != 0) {
            if (brook_cas_freeze_peer(&h->reader_alive)) {
                return -ERR_PROCESS_TERMINATED;
            }
            continue;
        }

        if (mode == BROOK_MODE_TRY) return -ERR_WOULD_BLOCK;

        if (mode == BROOK_MODE_TIMEOUT) {
            uint64_t now = clock_uptime_ms();
            if (now >= deadline_ms) return -ERR_TIMEOUT;

            brook_wait_cycle(&h->head, &spin, deadline_ms);
            continue;
        }

        brook_sleep_until(b, brook_push_ready);
    }
}

static int brook_pop_core(Brook *b, void *frame,
                          BrookWaitMode mode, uint64_t deadline_ms)
{
    if (!b || !frame)              return -ERR_INVALID_ARGS;
    if (b->role != BROOK_READER)   return -ERR_INVALID_ARGS;

    BrookHeaderUser *h = b->hdr;
    uint32_t cap = b->frame_count;
    uint32_t cap_mask = cap - 1;
    uint32_t fs = b->frame_size;

    uint32_t spin = 0;
    for (;;) {
        uint64_t head = brook_load_relaxed_u64(&h->head);
        uint64_t tail = brook_load_acquire_u64(&h->tail);

        if (head != tail) {
            const uint8_t *slot = b->slots + ((uint32_t)(head & cap_mask)) * fs;
            memcpy(frame, slot, fs);

            __atomic_exchange_n(&h->head, head + 1, __ATOMIC_SEQ_CST);

            brook_ring(&h->writer_bell);
            return 0;
        }

        if (!b->streaming &&
            brook_load_acquire_u32(&h->writer_alive) == 0 &&
            brook_load_acquire_u32(&h->writer_ever_attached) != 0) {
            if (brook_cas_freeze_peer(&h->writer_alive)) {
                return -ERR_STREAM_CLOSED;
            }
            continue;
        }

        if (mode == BROOK_MODE_TRY) return -ERR_WOULD_BLOCK;

        if (mode == BROOK_MODE_TIMEOUT) {
            uint64_t now = clock_uptime_ms();
            if (now >= deadline_ms) return -ERR_TIMEOUT;

            brook_wait_cycle(&h->tail, &spin, deadline_ms);
            continue;
        }

        brook_sleep_until(b, brook_pop_ready);
    }
}

int brook_push(Brook *b, const void *frame)
{
    return brook_push_core(b, frame, BROOK_MODE_BLOCK, 0);
}

int brook_try_push(Brook *b, const void *frame)
{
    return brook_push_core(b, frame, BROOK_MODE_TRY, 0);
}

int brook_push_timeout(Brook *b, const void *frame, uint32_t timeout_ms)
{
    if (timeout_ms == 0) return brook_push(b, frame);
    uint64_t deadline = clock_uptime_ms() + timeout_ms;
    return brook_push_core(b, frame, BROOK_MODE_TIMEOUT, deadline);
}

int brook_pop(Brook *b, void *frame)
{
    return brook_pop_core(b, frame, BROOK_MODE_BLOCK, 0);
}

int brook_try_pop(Brook *b, void *frame)
{
    return brook_pop_core(b, frame, BROOK_MODE_TRY, 0);
}

int brook_pop_timeout(Brook *b, void *frame, uint32_t timeout_ms)
{
    if (timeout_ms == 0) return brook_pop(b, frame);
    uint64_t deadline = clock_uptime_ms() + timeout_ms;
    return brook_pop_core(b, frame, BROOK_MODE_TIMEOUT, deadline);
}

int brook_bell_hang(Brook *b, uint32_t strand_pid)
{
    if (!b || !b->hdr)    return -ERR_INVALID_ARGS;
    if (strand_pid == 0)  return -ERR_INVALID_ARGS;

    __atomic_store_n(brook_own_bell(b->hdr, b->role), strand_pid, __ATOMIC_SEQ_CST);
    return 0;
}

int brook_bell_take(Brook *b)
{
    if (!b || !b->hdr) return -ERR_INVALID_ARGS;

    __atomic_store_n(brook_own_bell(b->hdr, b->role), 0u, __ATOMIC_RELEASE);
    return 0;
}

uint32_t brook_frame_size(const Brook *b)
{
    return b ? b->frame_size : 0;
}

uint32_t brook_frame_count(const Brook *b)
{
    return b ? b->frame_count : 0;
}

uint64_t brook_handle_header_va(const Brook *b)
{
    return b ? b->va_header : 0;
}

uint32_t brook_available(const Brook *b)
{
    if (!b) return 0;
    uint64_t tail = brook_load_acquire_u64(&b->hdr->tail);
    uint64_t head = brook_load_relaxed_u64(&b->hdr->head);
    uint64_t n = tail - head;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

uint32_t brook_free(const Brook *b)
{
    if (!b) return 0;
    uint64_t head = brook_load_acquire_u64(&b->hdr->head);
    uint64_t tail = brook_load_relaxed_u64(&b->hdr->tail);
    uint64_t used = tail - head;
    uint32_t cap = b->frame_count;
    return used >= cap ? 0 : (uint32_t)(cap - used);
}

bool brook_writer_ever_attached(const Brook *b)
{
    if (!b) return false;
    return brook_load_acquire_u32(&b->hdr->writer_ever_attached) != 0;
}