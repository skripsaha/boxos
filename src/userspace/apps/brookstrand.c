/*
 * brookstrand — cross-strand same-cabin Brook regression test.
 *
 * One cabin, two strands (writer on one, reader on the other). Guards that the
 * SPSC Brook substrate stays coherent AND race-free when both peers live in the
 * SAME cabin — the shape box::current / Ф24c rely on. Adversarial shapes:
 *
 *   T1  sibling = WRITER|CREATE pushes 8; main = READER pops 8.
 *   T2  single-frame "avail=1" shape: sibling writer pushes EXACTLY ONE frame,
 *       main reader try_pops it.
 *   T3  reader-opens-FIRST: main opens READER|CREATE; sibling opens WRITER and
 *       pushes; main drains.
 *   T4  reversed roles: main = WRITER|CREATE pushes; sibling = READER drains.
 *   T5  TRUE concurrent (the only shape that can expose a cross-core race —
 *       T1-T4 are serialized, post-hoc coherence only): barrier-synced
 *       SIMULTANEOUS brook_open on two cores (races the kernel global registry /
 *       per-cabin VA bump / role assign) + a 10k-frame overlapping push/pop
 *       hammer through a 16-slot ring; verifies every frame arrives once, in
 *       order, no loss/hang. Proven on bios1/bios16/uefi1/uefi16.
 *
 * T1-T4 print va_header + observed avail for both peers so any divergence shows.
 */

#include "box/print.h"
#include "box/brook.h"
#include "box/system.h"
#include "box/clock.h"            /* clock_uptime_ms — the ClockBoard, no syscall */
#include "box/sync.h"
#include "box/strand.h"
#include "box/cpu.h"
#include "box/core/strand_self.h"
#include "box/string.h"
#include "box/error.h"

#define FS       8u
#define FC       16u

static int g_pass = 0, g_total = 0;
static void ok(const char *n)  { g_pass++; g_total++; printf("[BS] %s PASS\n", n); }
static void no(const char *n, const char *w) { g_total++; printf("[BS] %s FAIL: %s\n", n, w); }

/* ---- shared rendezvous (same cabin AS, both strands see it) -------------- */
typedef struct {
    const char *tag;
    uint32_t    nframes;
    volatile uint64_t va_header;
    volatile uint32_t avail_after;
    volatile uint32_t pushed;
    volatile uint32_t popped;
    volatile uint64_t state;   /* 0=running 1=done 2=open-fail */
} Rdv;

/* sibling pushes nframes into a WRITER|CREATE brook, then parks alive. */
static void sib_writer(void *arg)
{
    Rdv *rv = (Rdv *)arg;
    Brook *w = brook_open(rv->tag, FS, FC, BROOK_WRITER | BROOK_CREATE);
    if (!w) { __atomic_store_n(&rv->state, 2u, __ATOMIC_RELEASE); addr_wake(&rv->state, 0); return; }
    rv->va_header = brook_handle_header_va(w);
    uint32_t n = 0;
    for (uint32_t i = 0; i < rv->nframes; i++) {
        uint8_t f[FS]; memset(f, 0, sizeof(f)); memcpy(f, &i, 4);
        if (brook_try_push(w, f) == 0) n++; else break;
    }
    __atomic_store_n(&rv->pushed, n, __ATOMIC_RELAXED);
    __atomic_store_n(&rv->avail_after, brook_available(w), __ATOMIC_RELAXED);
    __atomic_store_n(&rv->state, 1u, __ATOMIC_RELEASE);
    addr_wake(&rv->state, 0);
    for (;;) yield();                          /* keep writer + mapping live  */
}

/* sibling drains a READER brook (for the reversed-roles test). */
static void sib_reader(void *arg)
{
    Rdv *rv = (Rdv *)arg;
    /* One attempt decides. T4 — this role's only caller — creates the brook and
     * only then spawns this strand, so the brook exists before this strand does
     * and there is no race to lose: a refusal here is a real failure.
     *
     * What stood here was 200 retries separated by a 50000-iteration busy spin:
     * a wait for a fact that had already happened, charged to the CPU. If a
     * future caller ever spawns this role BEFORE creating the brook it must
     * publish that creation as a word this strand can park on, the way
     * stress_reader parks on the writer's `wopened` — never as a retry budget,
     * which burns out on a fast core long before a slow kernel commits. */
    Brook *r = brook_open(rv->tag, FS, FC, BROOK_READER);
    if (!r) { __atomic_store_n(&rv->state, 2u, __ATOMIC_RELEASE); addr_wake(&rv->state, 0); return; }
    rv->va_header = brook_handle_header_va(r);
    uint32_t got = 0;
    for (uint32_t i = 0; i < rv->nframes; i++) {
        uint8_t f[FS];
        int rc = brook_pop_timeout(r, f, 1000);
        if (rc != 0) break;
        got++;
    }
    __atomic_store_n(&rv->popped, got, __ATOMIC_RELAXED);
    __atomic_store_n(&rv->state, 1u, __ATOMIC_RELEASE);
    addr_wake(&rv->state, 0);
    brook_release(r);
    for (;;) yield();
}

/* Wait for the sibling's verdict. `state` leaving 0 is a FACT that is coming:
 * every path through sib_writer and sib_reader stores 1 or 2 before it returns,
 * and their brook calls all carry their own deadlines, so neither can end
 * without speaking. The park is therefore unbounded and lost-wake-safe (a store
 * that landed first returns VALUE_MISMATCH without sleeping). A sibling that
 * never speaks is a wedge, and a parked main is exactly how Nightwatch proves
 * one — the 400 parks of 50 ms this replaces made it a twenty-second guess that
 * printed "writer timeout" about a host that was merely loaded.
 *
 * Returns 0 when the sibling did its work, -2 when it could not open the brook. */
static int wait_done(Rdv *rv)
{
    while (__atomic_load_n(&rv->state, __ATOMIC_ACQUIRE) == 0) {
        error_t prc = addr_park(&rv->state, 0, 0);
        if (prc != OK && prc != ERR_ADDR_VALUE_MISMATCH)
            yield();     /* park unavailable — stay correct the slow way */
    }
    return (__atomic_load_n(&rv->state, __ATOMIC_ACQUIRE) == 2u) ? -2 : 0;
}

/* main = READER for a sibling writer; verify it drains `pushed`. */
static void main_reads(const char *name, Rdv *rv)
{
    if (wait_done(rv) == -2) { no(name, "sibling writer open failed"); return; }

    uint32_t pushed = __atomic_load_n(&rv->pushed, __ATOMIC_RELAXED);
    uint32_t wav    = __atomic_load_n(&rv->avail_after, __ATOMIC_RELAXED);

    Brook *r = brook_open(rv->tag, FS, FC, BROOK_READER);
    if (!r) { no(name, "reader open NULL"); return; }
    uint32_t rav = brook_available(r);
    printf("[BS] %s writer_va=0x%lx pushed=%u wavail=%u | reader_va=0x%lx ravail=%u\n",
           name, (unsigned long)rv->va_header, pushed, wav,
           (unsigned long)brook_handle_header_va(r), rav);

    uint32_t got = 0;
    for (uint32_t i = 0; i < pushed; i++) {
        uint8_t f[FS];
        if (brook_try_pop(r, f) != 0) break;
        uint32_t idx = 0; memcpy(&idx, f, 4);
        if (idx != i) break;
        got++;
    }
    brook_release(r);
    if (got == pushed && pushed == rv->nframes) ok(name);
    else { printf("[BS] %s popped=%u expected=%u\n", name, got, pushed); no(name, "reader did not receive frames"); }
}

/* ---- T5: TRUE concurrent stress (the test bios1 CANNOT expose) ------------
 * T1-T4 are SERIALIZED (writer finishes, THEN reader reads) — they prove post-hoc
 * coherence but never run the two strands at once. T5 is the real 16c test:
 *   1. BARRIER — writer-strand and reader-strand each signal "ready" and spin on
 *      `go`; main releases them together so both charge brook_open SIMULTANEOUSLY
 *      (races the kernel global tag-registry insert-vs-lookup, the per-cabin VA
 *      bump allocator, and writer/reader role assignment on two cores).
 *   2. HAMMER — writer pushes STRESS_FRAMES through a tiny FC=16 ring while the
 *      reader drains concurrently (~STRESS_FRAMES/16 block-on-full / block-on-
 *      empty wake cycles), exposing hot-path lost-wakeup and head/tail ordering
 *      under true parallelism.
 * Bounded everywhere (push/pop timeouts + a main watchdog) so a stuck peer is a
 * FAIL, never an infinite hang. Verifies EVERY frame arrives exactly once, in
 * order: a concurrent-open divergence (different rings) makes popped=0; a hot-
 * path race makes count mismatch or order_ok=0. */
#define STRESS_FRAMES  10000u

typedef struct {
    const char       *tag;
    volatile uint32_t wready, rready;   /* each strand parked AT the barrier      */
    volatile uint64_t go;               /* main releases both → concurrent open   */
    /* wopened is 64-bit because the READER PARKS on it when it loses the open
     * race: addr_park compares the parked-on word as a uint64_t and buckets it
     * at 8-byte granularity (box/sync.h), so a 32-bit word would have the
     * kernel compare the bytes behind it. ropened has no waiter. */
    volatile uint64_t wopened;          /* 0 running, 1 ok, 2 fail                */
    volatile uint32_t ropened;          /* 0 running, 1 ok, 2 fail                */
    volatile uint32_t pushed, popped;
    volatile uint32_t order_ok;         /* 1 iff every popped idx == expected      */
    volatile uint64_t done;             /* reader finished                         */
} Stress;

/* Everything T5 publishes, folded into one word. wopened and ropened are 0/1/2
 * and pushed and popped never exceed STRESS_FRAMES, which fits in fourteen
 * bits, so the four sit side by side with no overlap and no loss. A change in
 * any of them is proof the pair is moving and restarts the watchdog's clock. */
static uint64_t t5_progress(Stress *s)
{
    return  (uint64_t)__atomic_load_n(&s->wopened, __ATOMIC_ACQUIRE)
         | ((uint64_t)__atomic_load_n(&s->ropened, __ATOMIC_ACQUIRE) <<  2)
         | ((uint64_t)__atomic_load_n(&s->pushed,  __ATOMIC_ACQUIRE) <<  4)
         | ((uint64_t)__atomic_load_n(&s->popped,  __ATOMIC_ACQUIRE) << 32);
}

/* How often the watchdog LOOKS — not what it decides. Nothing wakes `done`
 * except the reader's own store, so this park normally runs to its own length;
 * its size sets the granularity of the answer and nothing else. */
#define T5_LOOK_MS   50u

/* How long T5 may say NOTHING AT ALL before the watchdog convicts. Any of the
 * four things it publishes moving restarts it (t5_progress above), so this is a
 * SILENCE budget — but T5 is also the one place in this file where silence can
 * be lawful for a long stretch: between the two opens and the two final counts
 * the hammer publishes nothing at all, so the budget has to outlast a whole
 * lawful run of 10000 frames through a 16-slot ring.
 *
 * It stays BOUNDED, alone in this file, because it is the only thing that can
 * end T5 in the doubly-broken case: stress_reader's park on `wopened` is
 * deliberately unbounded — the writer's published open is the one fact that can
 * lawfully end it — so a writer that never reaches that store leaves the reader
 * asleep and `done` forever 0, with nothing else in the program to notice.
 *
 * The figure is the one this watchdog always meant: the cap it replaces was
 * 1200 parks of 50 ms, sixty seconds. Counted in parks that was a lie in both
 * directions — a park broken early by a spurious wake spent a turn without
 * spending the time — while measured on the ClockBoard it is sixty real
 * seconds. It is also twelve times the 5000 ms deadline every blocking step in
 * T5 carries (brook_push_timeout / brook_pop_timeout below), so a peer that is
 * merely stuck always convicts itself, and its partner after it, long before
 * the watchdog has to. */
#define T5_STILL_MS  60000u

/* Barrier wait: tight pause-spin on multicore (so two strands charge brook_open
 * within the same narrow window), but yield every 64 spins so a SINGLE App-Core
 * still hands the core back to main to release `go` (a pure busy-spin deadlocks
 * on 1c). */
static void bs_barrier_wait(volatile uint64_t *go)
{
    uint32_t spins = 0;
    while (__atomic_load_n(go, __ATOMIC_ACQUIRE) == 0) {
        if (++spins < 64u) __asm__ volatile("pause" ::: "memory");
        else { spins = 0; yield(); }
    }
}

static void stress_writer(void *arg)
{
    Stress *s = (Stress *)arg;
    __atomic_store_n(&s->wready, 1u, __ATOMIC_RELEASE);
    bs_barrier_wait(&s->go);
    Brook *w = brook_open(s->tag, FS, FC, BROOK_WRITER | BROOK_CREATE);
    /* Either way the answer is published AND woken: this store is the only fact
     * that can end a reader that lost the open race (see stress_reader). */
    if (!w) { __atomic_store_n(&s->wopened, 2u, __ATOMIC_RELEASE);
              addr_wake(&s->wopened, 0); for(;;) yield(); }
    __atomic_store_n(&s->wopened, 1u, __ATOMIC_RELEASE);
    addr_wake(&s->wopened, 0);
    uint32_t n = 0;
    for (uint32_t i = 0; i < STRESS_FRAMES; i++) {
        uint8_t f[FS]; memset(f, 0, sizeof(f)); memcpy(f, &i, 4);
        if (brook_push_timeout(w, f, 5000) != 0) break;   /* stuck reader → FAIL not hang */
        n++;
    }
    __atomic_store_n(&s->pushed, n, __ATOMIC_RELEASE);
    brook_release(w);
    for (;;) yield();
}

static void stress_reader(void *arg)
{
    Stress *s = (Stress *)arg;
    __atomic_store_n(&s->rready, 1u, __ATOMIC_RELEASE);
    bs_barrier_wait(&s->go);
    /* THE charge — the one this test exists for. It leaves the barrier with
     * nothing in front of it, so it lands in the same narrow window as the
     * writer's and races the kernel registry insert-vs-lookup on two cores. */
    Brook *r = brook_open(s->tag, FS, FC, BROOK_READER);
    /* Losing that race is lawful: until the writer's CREATE returns there is no
     * brook to attach to. What ends the wait is the writer saying its open is
     * DONE — a fact, published and woken — not a retry budget. The budget was
     * 2000 yields, and a yield on an idle core returns in well under a
     * microsecond, so the whole of it could burn out before the writer had even
     * entered brook_open: the test then reported a concurrent-open failure the
     * kernel had never had a chance to commit. The park is lost-wake-safe (a
     * store that lands first makes it return VALUE_MISMATCH without sleeping),
     * so it has no clock of its own. */
    if (!r) {
        uint64_t wo;
        while ((wo = __atomic_load_n(&s->wopened, __ATOMIC_ACQUIRE)) == 0u) {
            error_t prc = addr_park(&s->wopened, 0u, 0);
            if (prc != OK && prc != ERR_ADDR_VALUE_MISMATCH)
                yield();     /* park unavailable — stay correct the slow way */
        }
        /* wo == 2: the writer could not create it, so nothing will ever appear.
         * wo == 1: it exists — one attempt decides, and a refusal now is a real
         * failure rather than a lost race. */
        if (wo == 1u) r = brook_open(s->tag, FS, FC, BROOK_READER);
    }
    if (!r) { __atomic_store_n(&s->ropened, 2u, __ATOMIC_RELEASE);
              __atomic_store_n(&s->done, 1u, __ATOMIC_RELEASE); addr_wake(&s->done, 0); for(;;) yield(); }
    __atomic_store_n(&s->ropened, 1u, __ATOMIC_RELEASE);
    uint32_t got = 0, order = 1;
    for (uint32_t i = 0; i < STRESS_FRAMES; i++) {
        uint8_t f[FS];
        if (brook_pop_timeout(r, f, 5000) != 0) break;    /* stuck/lost writer → FAIL not hang */
        uint32_t idx = 0; memcpy(&idx, f, 4);
        if (idx != i) order = 0;                          /* loss / dup / reorder */
        got++;
    }
    __atomic_store_n(&s->order_ok, order, __ATOMIC_RELEASE);
    __atomic_store_n(&s->popped, got, __ATOMIC_RELEASE);
    brook_release(r);
    __atomic_store_n(&s->done, 1u, __ATOMIC_RELEASE);
    addr_wake(&s->done, 0);
    for (;;) yield();
}

int main(void)
{
    printf("[BS] brookstrand start\n");
    if (!cpu_has_fsgsbase()) { printf("[BS] SKIP: needs FSGSBASE (STRICT)\n"); exit(0); }

    /* T1 — sibling writer 8, main reader. */
    { static Rdv rv; memset(&rv,0,sizeof rv); rv.tag="brook:strand:t1"; rv.nframes=8;
      if (strand_spawn(sib_writer, &rv)==0) no("T1","spawn"); else main_reads("T1", &rv); }

    /* T2 — the original avail=1 shape: sibling writer ONE frame. */
    { static Rdv rv; memset(&rv,0,sizeof rv); rv.tag="brook:strand:t2"; rv.nframes=1;
      if (strand_spawn(sib_writer, &rv)==0) no("T2","spawn"); else main_reads("T2", &rv); }

    /* T3 — reader-creates-first: main opens READER|CREATE; sibling writer attaches. */
    { static Rdv rv; memset(&rv,0,sizeof rv); rv.tag="brook:strand:t3"; rv.nframes=8;
      Brook *r = brook_open(rv.tag, FS, FC, BROOK_READER | BROOK_CREATE);
      if (!r) { no("T3","reader create"); }
      else {
        if (strand_spawn(sib_writer, &rv)==0) { no("T3","spawn"); brook_release(r); }
        else {
          if (wait_done(&rv) != 0) { no("T3", "sibling writer open failed (BUSY?)"); brook_release(r); }
          else {
            uint32_t pushed = __atomic_load_n(&rv.pushed, __ATOMIC_RELAXED);
            printf("[BS] T3 writer_va=0x%lx pushed=%u wavail=%u | reader_va=0x%lx ravail=%u\n",
                   (unsigned long)rv.va_header, pushed,
                   __atomic_load_n(&rv.avail_after,__ATOMIC_RELAXED),
                   (unsigned long)brook_handle_header_va(r), brook_available(r));
            uint32_t got=0; for (uint32_t i=0;i<pushed;i++){ uint8_t f[FS]; if (brook_try_pop(r,f)!=0) break; got++; }
            brook_release(r);
            if (got==pushed && pushed==rv.nframes) ok("T3"); else { printf("[BS] T3 popped=%u expected=%u\n",got,pushed); no("T3","reader did not receive"); }
          }
        }
      }
    }

    /* T4 — reversed: main WRITER|CREATE pushes, sibling READER drains. */
    { static Rdv rv; memset(&rv,0,sizeof rv); rv.tag="brook:strand:t4"; rv.nframes=8;
      Brook *w = brook_open(rv.tag, FS, FC, BROOK_WRITER | BROOK_CREATE);
      if (!w) { no("T4","writer create"); }
      else if (strand_spawn(sib_reader, &rv)==0) { no("T4","spawn"); brook_release(w); }
      else {
        printf("[BS] T4 main(writer) va=0x%lx\n", (unsigned long)brook_handle_header_va(w));
        uint32_t n=0; for (uint32_t i=0;i<rv.nframes;i++){ uint8_t f[FS]; memset(f,0,sizeof f); memcpy(f,&i,4);
          if (brook_push_timeout(w, f, 2000)==0) n++; else break; }
        int rc = wait_done(&rv);
        brook_release(w);
        if (rc!=0) { no("T4", "sibling reader open failed"); }
        else {
          uint32_t got = __atomic_load_n(&rv.popped,__ATOMIC_RELAXED);
          printf("[BS] T4 main pushed=%u | sibling(reader) va=0x%lx popped=%u\n",
                 n, (unsigned long)rv.va_header, got);
          if (got==n && n==rv.nframes) ok("T4"); else no("T4","sibling reader did not receive");
        }
      }
    }

    /* T5 — concurrent open-race + overlapping hammer (the test bios1 CANNOT
     * expose: needs two strands running on two cores at once). */
    {
        static Stress s; memset(&s, 0, sizeof s); s.tag = "brook:strand:t5";
        uint32_t wp = strand_spawn(stress_writer, &s);
        uint32_t rp = strand_spawn(stress_reader, &s);
        if (wp == 0 || rp == 0) { no("T5", "spawn"); }
        else {
            /* Hold BOTH strands at the barrier, then release together so their
             * brook_open calls race on two cores. */
            uint32_t bc = 0;
            while (!(__atomic_load_n(&s.wready, __ATOMIC_ACQUIRE) &&
                     __atomic_load_n(&s.rready, __ATOMIC_ACQUIRE))) {
                if (++bc > 4000u) break;
                yield();
            }
            __atomic_store_n(&s.go, 1u, __ATOMIC_RELEASE);          /* concurrent open */
            /* Watchdog: a stuck peer FAILS the test, never hangs the suite. The
             * park is the sleep; what convicts is the CLOCK saying T5 has
             * published nothing for T5_STILL_MS, not a tally of how many times
             * main parked. */
            uint64_t started = clock_uptime_ms();
            uint64_t mark    = t5_progress(&s);
            int timedout = 0;
            while (__atomic_load_n(&s.done, __ATOMIC_ACQUIRE) == 0) {
                addr_park(&s.done, 0, T5_LOOK_MS);
                uint64_t now = t5_progress(&s);
                if (now != mark) { mark = now; started = clock_uptime_ms(); }
                else if (clock_uptime_ms() - started >= T5_STILL_MS) { timedout = 1; break; }
            }
            uint64_t wo = __atomic_load_n(&s.wopened, __ATOMIC_ACQUIRE);
            uint32_t ro = __atomic_load_n(&s.ropened, __ATOMIC_ACQUIRE);
            uint32_t pu = __atomic_load_n(&s.pushed,  __ATOMIC_ACQUIRE);
            uint32_t po = __atomic_load_n(&s.popped,  __ATOMIC_ACQUIRE);
            uint32_t od = __atomic_load_n(&s.order_ok, __ATOMIC_ACQUIRE);
            printf("[BS] T5 concurrent: wopened=%lu ropened=%u pushed=%u popped=%u order_ok=%u timedout=%d (expect %u)\n",
                   (unsigned long)wo, ro, pu, po, od, timedout, STRESS_FRAMES);
            if (timedout)                no("T5", "concurrent hammer HUNG");
            else if (wo != 1 || ro != 1) no("T5", "concurrent brook_open FAILED");
            else if (pu != STRESS_FRAMES || po != STRESS_FRAMES) no("T5", "frame loss under concurrent hammer");
            else if (!od)                no("T5", "out-of-order / dup under concurrent hammer");
            else ok("T5");
        }
    }

    printf("[BS] %d/%d\n", g_pass, g_total);
    printf(g_pass==g_total ? "[BS] PASS\n" : "[BS] FAIL\n");
    exit(g_pass==g_total ? 0 : 1);
    return 0;
}
