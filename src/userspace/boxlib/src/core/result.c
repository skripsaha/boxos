#include "box/core/result.h"
#include "box/core/notify.h"
#include "box/system.h"
#include "box/cpu.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/core/stash.h"
#include "box/debug.h"
#include "box/turnin.h"

bool result_available(void) {
    ResultRing* rr = result_ring();
    __sync_synchronize();
    return rr->hdr.head != rr->hdr.tail;
}

static bool head_slot(ResultRing *rr, ResultSlot **slot, uint64_t *pos,
                      uint64_t *tail, uint64_t *expected)
{
    uint32_t cap    = __atomic_load_n(&rr->hdr.slot_count_max, __ATOMIC_RELAXED);
    uint64_t base   = __atomic_load_n(&rr->hdr.slots_base,     __ATOMIC_RELAXED);
    uint32_t stride = __atomic_load_n(&rr->hdr.slot_size,      __ATOMIC_RELAXED);
    if (cap == 0 || base == 0 || stride == 0)        return false;
    if (base < 0x100000000ULL)                       return false;
    if (stride != sizeof(ResultSlot))                return false;

    *tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
    *pos  = __atomic_load_n(&rr->hdr.head, __ATOMIC_RELAXED);
    if (*pos == *tail) return false;

    *slot     = (ResultSlot *)(uintptr_t)(base + (*pos % cap) * stride);
    *expected = 2u * (*pos / (uint64_t)cap) + 1u;
    return true;
}

bool result_published_at_head(void) {
    ResultRing* rr = result_ring();
    if (!rr) return false;
    ResultSlot *slot; uint64_t pos, tail, expected;
    if (!head_slot(rr, &slot, &pos, &tail, &expected)) return false;
    return __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) == expected;
}

bool result_peek(Result* out) {
    ResultRing* rr = result_ring();
    if (!rr || !out) return false;
    ResultSlot *slot; uint64_t pos, tail, expected;
    if (!head_slot(rr, &slot, &pos, &tail, &expected)) return false;
    if (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) != expected) return false;
    *out = slot->r;
    return true;
}

uint32_t result_count(void) {
    ResultRing* rr = result_ring();
    __sync_synchronize();
    uint64_t n = rr->hdr.tail - rr->hdr.head;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

static volatile uint32_t g_rp_calls;
static volatile uint32_t g_rp_empty;
static volatile uint32_t g_rp_seq_mismatch;
static volatile uint32_t g_rp_success;
static volatile uint64_t g_rp_last_seq_seen;
static volatile uint64_t g_rp_last_expected;
static volatile uint64_t g_rp_last_pos;
static volatile uint64_t g_rp_last_tail;

void result_pop_stats(uint64_t out[8]) {
    out[0] = __atomic_load_n(&g_rp_calls,         __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&g_rp_empty,         __ATOMIC_RELAXED);
    out[2] = __atomic_load_n(&g_rp_seq_mismatch,  __ATOMIC_RELAXED);
    out[3] = __atomic_load_n(&g_rp_success,       __ATOMIC_RELAXED);
    out[4] = __atomic_load_n(&g_rp_last_seq_seen, __ATOMIC_RELAXED);
    out[5] = __atomic_load_n(&g_rp_last_expected, __ATOMIC_RELAXED);
    out[6] = __atomic_load_n(&g_rp_last_pos,      __ATOMIC_RELAXED);
    out[7] = __atomic_load_n(&g_rp_last_tail,     __ATOMIC_RELAXED);
}

bool result_pop(Result* out) {
    ResultRing* rr = result_ring();
    if (!rr || !out) return false;
    __atomic_add_fetch(&g_rp_calls, 1, __ATOMIC_RELAXED);

    ResultSlot *slot; uint64_t pos = 0, tail = 1, expected;
    if (!head_slot(rr, &slot, &pos, &tail, &expected)) {
        if (pos == tail) __atomic_add_fetch(&g_rp_empty, 1, __ATOMIC_RELAXED);
        return false;
    }

    uint64_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
    if (seq != expected) {
        __atomic_store_n(&g_rp_last_seq_seen, seq, __ATOMIC_RELAXED);
        __atomic_store_n(&g_rp_last_expected, expected, __ATOMIC_RELAXED);
        __atomic_store_n(&g_rp_last_pos, pos, __ATOMIC_RELAXED);
        __atomic_store_n(&g_rp_last_tail, tail, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_rp_seq_mismatch, 1, __ATOMIC_RELAXED);
        return false;
    }
    __atomic_add_fetch(&g_rp_success, 1, __ATOMIC_RELAXED);

    *out = slot->r;

    __atomic_store_n(&slot->seq, expected + 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&rr->hdr.head, pos + 1u, __ATOMIC_RELEASE);
    return true;
}

static Stash g_ipc_stash;
static Stash g_non_ipc_stash;
static Stash g_ferry_stash;

static Stash *stash_of(uint64_t *slot, Stash *main_stash, bool create)
{
    Stash *s = main_stash;
    if (slot) {
        s = (Stash *)(uintptr_t)*slot;
        if (!s) {
            if (!create) return NULL;
            s = malloc(sizeof(Stash));
            if (!s) return NULL;
            memset(s, 0, sizeof(*s));
            *slot = (uint64_t)(uintptr_t)s;
        }
    }
    if (s->entry_size == 0) {
        ResultRing *rr = result_ring();
        uint32_t cap = rr ? __atomic_load_n(&rr->hdr.slot_count_max, __ATOMIC_RELAXED) : 0;
        stash_init(s, sizeof(Result), cap);
    }
    return s;
}

static Stash *ipc_stash_self(bool create) {
    StrandInfo *si = strand_info_or_null();
    return stash_of(si ? &si->ipc_stash_ptr : NULL, &g_ipc_stash, create);
}
static Stash *non_ipc_stash_self(bool create) {
    StrandInfo *si = strand_info_or_null();
    return stash_of(si ? &si->non_ipc_stash_ptr : NULL, &g_non_ipc_stash, create);
}
static Stash *ferry_stash_self(bool create) {
    StrandInfo *si = strand_info_or_null();
    return stash_of(si ? &si->ferry_stash_ptr : NULL, &g_ferry_stash, create);
}

enum {
    FOR_CALLER = 1u << 0,
    FOR_IPC    = 1u << 1,
    FOR_FERRY  = 1u << 2,
    FOR_NOBODY = 1u << 3,
};

static uint32_t route_of(const Result *e)
{
    if (e->sender_pid != 0)                       return FOR_IPC;
    if (KCTX_KIND(e->context) == KCTX_STORAGE)    return FOR_FERRY;
    if (KCTX_KIND(e->context) == KCTX_TOUCH)      return FOR_NOBODY;
    if (e->error_code == 9 ) return FOR_NOBODY;
    return FOR_CALLER;
}

static Stash *stash_for(uint32_t route)
{
    switch (route) {
    case FOR_IPC:    return ipc_stash_self(true);
    case FOR_FERRY:  return ferry_stash_self(true);
    case FOR_CALLER: return non_ipc_stash_self(true);
    default:         return NULL;
    }
}

static const char *name_of(uint32_t route)
{
    switch (route) {
    case FOR_IPC:    return "an IPC message";
    case FOR_FERRY:  return "a ferry completion";
    default:         return "a kernel reply";
    }
}

static bool g_stash_no_room_said;

static void stash_no_room(uint32_t route)
{
    if (g_stash_no_room_said) return;
    g_stash_no_room_said = true;
    char line[160];
    size_t n = 0;
    const char *parts[3] = { "[stash] DEFECT: no memory to keep ", name_of(route),
                             " - it stays in the ring, and this strand cannot reach what is behind it" };
    for (int i = 0; i < 3; i++) {
        size_t l = strlen(parts[i]);
        if (n + l >= sizeof(line)) l = sizeof(line) - 1 - n;
        memcpy(line + n, parts[i], l);
        n += l;
    }
    line[n] = '\0';
    kdbg_nowait(line);
}

static int ring_take(uint32_t want, uint32_t drop, bool one_step, Result *out)
{
    Result e;
    while (result_peek(&e)) {
        uint32_t r = route_of(&e);
        if (r & want) {
            if (!result_pop(&e)) return 0;
            *out = e;
            return 1;
        }
        if (r & (drop | FOR_NOBODY)) {
            if (!result_pop(&e)) return 0;
            continue;
        }
        Stash *st = stash_for(r);
        if (!st || !stash_reserve(st)) { stash_no_room(r); return -1; }
        if (!result_pop(&e)) return 0;
        stash_put(st, &e);
        if (one_step) return 2;
    }
    return 0;
}

bool result_pop_non_ipc(Result* out) {
    if (!out) return false;
    if (stash_take(non_ipc_stash_self(false), out)) return true;
    return ring_take(FOR_CALLER, 0, false, out) == 1;
}

bool result_pop_ipc(Result* out) {
    if (!out) return false;
    if (stash_take(ipc_stash_self(false), out)) return true;
    return ring_take(FOR_IPC, 0, false, out) == 1;
}

bool result_pop_ferry(Result* out) {
    if (!out) return false;
    if (stash_take(ferry_stash_self(false), out)) return true;
    return ring_take(FOR_FERRY, 0, false, out) == 1;
}

bool result_pop_any(Result* out) {
    if (!out) return false;
    if (result_pop_ipc(out))     return true;
    if (result_pop_non_ipc(out)) return true;
    return ring_take(FOR_IPC | FOR_CALLER, 0, false, out) == 1;
}

void result_restash(const Result* r) {
    if (!r) return;
    uint32_t route = route_of(r);
    if (route == FOR_NOBODY) return;
    Stash *st = stash_for(route);
    if (!st || !stash_reserve(st)) { stash_no_room(route); return; }
    stash_put(st, r);
}

uint32_t result_ipc_stash_count(void)     { return stash_count(ipc_stash_self(false)); }
uint32_t result_non_ipc_stash_count(void) { return stash_count(non_ipc_stash_self(false)); }
uint32_t result_ferry_stash_count(void)   { return stash_count(ferry_stash_self(false)); }

void result_stash_free_self(void) {
    StrandInfo *si = strand_info_or_null();
    if (!si) return;
    uint64_t *slots[3] = { &si->ipc_stash_ptr, &si->non_ipc_stash_ptr, &si->ferry_stash_ptr };
    for (int i = 0; i < 3; i++) {
        Stash *s = (Stash *)(uintptr_t)*slots[i];
        if (!s) continue;
        stash_free(s);
        free(s);
        *slots[i] = 0;
    }
}

bool result_pop_touch(Result* out) {
    (void)out;
    return false;
}

void result_drain_orphan_replies(void) {
    stash_clear(non_ipc_stash_self(false));
    Result e;
    (void)ring_take(0, FOR_CALLER, false, &e);
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static bool result_wait_umwait(Result* out, uint32_t timeout_ms) {
    ResultRing* rr = result_ring();
    volatile uint64_t *tail_addr = (volatile uint64_t *)
        ((uintptr_t)rr + OFFSETOF(ResultRing, hdr.tail));

    while (1) {
        __sync_synchronize();

        if (result_available()) {
            if (result_pop_non_ipc(out)) return true;
        }

        umonitor((volatile void*)tail_addr);
        __sync_synchronize();

        if (!result_available()) {
            uint64_t deadline_tsc;
            if (timeout_ms == 0) {
                deadline_tsc = 0xFFFFFFFFFFFFFFFFULL;
            } else {
                uint64_t tsc_now   = rdtsc();
                uint64_t tsc_delta = cpu_ms_to_tsc(timeout_ms);
                deadline_tsc = tsc_now + tsc_delta;
            }

            int wake_reason = umwait(0, deadline_tsc);

            if (wake_reason == 1 && timeout_ms > 0) {
                __sync_synchronize();
                if (!result_available()) return false;
            }
        }
    }
}

static bool result_wait_yield(Result* out, uint32_t timeout_ms) {
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    }

    while (1) {
        __sync_synchronize();

        if (result_available()) {
            if (result_pop_non_ipc(out)) return true;
        }

        if (timeout_ms > 0 && rdtsc() >= deadline) return false;

        __asm__ volatile("pause");
    }
}

static bool result_wait_raw(Result* out, uint32_t timeout_ms) {
    if (!out) return false;
    stash_clear(non_ipc_stash_self(false));
    if (result_pop_non_ipc(out)) return true;

    if (cpu_has_waitpkg()) return result_wait_umwait(out, timeout_ms);
    return result_wait_yield(out, timeout_ms);
}

static uint64_t g_reply_orphans_dropped;

uint64_t result_orphans_dropped(void)
{
    return __atomic_load_n(&g_reply_orphans_dropped, __ATOMIC_RELAXED);
}

static bool result_wait_inner(Result* out, uint32_t expect_cookie, uint32_t timeout_ms) {
    if (!out) return false;

    uint64_t deadline_tsc = 0;
    if (timeout_ms > 0) deadline_tsc = rdtsc() + cpu_ms_to_tsc(timeout_ms);

    for (;;) {
        uint32_t remaining_ms = timeout_ms;
        if (timeout_ms > 0) {
            uint64_t now = rdtsc();
            if (now >= deadline_tsc) return false;
            remaining_ms = (uint32_t)cpu_tsc_to_ms(deadline_tsc - now);
            if (remaining_ms == 0) remaining_ms = 1;
        }
        if (!result_wait_raw(out, remaining_ms)) return false;
        if (KCTX_COOKIE24(out->context) == expect_cookie) return true;
        __atomic_add_fetch(&g_reply_orphans_dropped, 1u, __ATOMIC_RELAXED);
    }
}

bool result_wait(Result* out, uint32_t expect_cookie, uint32_t timeout_ms) {
    ResultRing* rr = result_ring();
    if (rr) __atomic_store_n(&rr->hdr.awaiting, (uint64_t)expect_cookie,
                             __ATOMIC_RELEASE);
    bool ok = result_wait_inner(out, expect_cookie, timeout_ms);
    if (rr) __atomic_store_n(&rr->hdr.awaiting, 0u, __ATOMIC_RELEASE);
    return ok;
}

bool result_wait_any(Result* out, uint32_t timeout_ms) {
    if (!out) return false;
    if (result_pop_ipc(out))     return true;
    if (result_pop_non_ipc(out)) return true;
    {
        int r = ring_take(FOR_IPC | FOR_CALLER, 0, true, out);
        if (r == 1) return true;
        if (r != 0) return false;
    }

    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    }

    while (1) {
        __sync_synchronize();

        if (result_available()) {
            int r = ring_take(FOR_IPC | FOR_CALLER, 0, true, out);
            if (r == 1) return true;
            if (r != 0) return false;
        }

        if (timeout_ms > 0 && rdtsc() >= deadline) return false;

        __asm__ volatile("pause");
    }
}

void yield(void);

static bool result_wait_ipc_umwait(Result* out, uint32_t timeout_ms) {
    ResultRing* rr = result_ring();
    volatile uint64_t *tail_addr = (volatile uint64_t *)
        ((uintptr_t)rr + OFFSETOF(ResultRing, hdr.tail));
    while (1) {
        __sync_synchronize();
        if (result_available() || result_ipc_stash_count() > 0) {
            if (result_pop_ipc(out)) return true;
        }
        umonitor((volatile void*)tail_addr);
        __sync_synchronize();
        if (!result_available() && result_ipc_stash_count() == 0) {
            uint64_t deadline_tsc;
            if (timeout_ms == 0) {
                deadline_tsc = 0xFFFFFFFFFFFFFFFFULL;
            } else {
                deadline_tsc = rdtsc() + cpu_ms_to_tsc(timeout_ms);
            }
            int wake_reason = umwait(0, deadline_tsc);
            if (wake_reason == 1 && timeout_ms > 0) {
                __sync_synchronize();
                if (!result_available() && result_ipc_stash_count() == 0)
                    return false;
            }
        }
    }
}

static bool result_wait_ipc_yield(Result* out, uint32_t timeout_ms) {
    uint64_t deadline = 0;
    if (timeout_ms > 0) deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    while (1) {
        __sync_synchronize();
        if (result_available() || result_ipc_stash_count() > 0) {
            if (result_pop_ipc(out)) return true;
        }
        if (timeout_ms > 0 && rdtsc() >= deadline) return false;
        yield();
    }
}

static bool result_wait_ipc_turnin(Result* out) {
    for (;;) {
        TurnInMark mark = box_mark();
        __sync_synchronize();
        if (result_available() || result_ipc_stash_count() > 0) {
            if (result_pop_ipc(out)) return true;
        }
        box_turn_in(mark);
    }
}

bool result_wait_ipc(Result* out, uint32_t timeout_ms) {
    if (!out) return false;
    if (result_pop_ipc(out)) return true;
    if (timeout_ms == 0)   return result_wait_ipc_turnin(out);
    if (cpu_has_waitpkg()) return result_wait_ipc_umwait(out, timeout_ms);
    return result_wait_ipc_yield(out, timeout_ms);
}

static bool result_wait_ferry_umwait(Result* out, uint32_t timeout_ms) {
    ResultRing* rr = result_ring();
    volatile uint64_t *tail_addr = (volatile uint64_t *)
        ((uintptr_t)rr + OFFSETOF(ResultRing, hdr.tail));
    while (1) {
        __sync_synchronize();
        if (stash_take(ferry_stash_self(false), out)) return true;
        if (result_available()) {
            int r = ring_take(FOR_FERRY, 0, true, out);
            if (r == 1) return true;
            if (r != 0) return false;
        }
        umonitor((volatile void*)tail_addr);
        __sync_synchronize();
        if (!result_available() && result_ferry_stash_count() == 0) {
            uint64_t deadline_tsc = (timeout_ms == 0)
                ? 0xFFFFFFFFFFFFFFFFULL
                : rdtsc() + cpu_ms_to_tsc(timeout_ms);
            int wake_reason = umwait(0, deadline_tsc);
            if (wake_reason == 1 && timeout_ms > 0) {
                __sync_synchronize();
                if (!result_available() && result_ferry_stash_count() == 0) return false;
            }
        }
    }
}

static bool result_wait_ferry_yield(Result* out, uint32_t timeout_ms) {
    uint64_t deadline = 0;
    if (timeout_ms > 0) deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    while (1) {
        __sync_synchronize();
        if (stash_take(ferry_stash_self(false), out)) return true;
        if (result_available()) {
            int r = ring_take(FOR_FERRY, 0, true, out);
            if (r == 1) return true;
            if (r != 0) return false;
        }
        if (timeout_ms > 0 && rdtsc() >= deadline) return false;
        yield();
    }
}

bool result_wait_ferry(Result* out, uint32_t timeout_ms) {
    if (!out) return false;
    if (stash_take(ferry_stash_self(false), out)) return true;
    if (cpu_has_waitpkg()) return result_wait_ferry_umwait(out, timeout_ms);
    return result_wait_ferry_yield(out, timeout_ms);
}
