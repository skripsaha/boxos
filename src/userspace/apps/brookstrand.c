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
    /* spin until the creator (main) has made the brook */
    Brook *r = 0;
    for (int t = 0; t < 200 && !r; t++) { r = brook_open(rv->tag, FS, FC, BROOK_READER); if (!r) { uint8_t junk; (void)junk; for (volatile int k=0;k<50000;k++){} } }
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

static int wait_done(Rdv *rv)
{
    uint32_t cyc = 0;
    while (__atomic_load_n(&rv->state, __ATOMIC_ACQUIRE) == 0) {
        if (++cyc > 400u) return -1;
        addr_park(&rv->state, 0, 50);
    }
    return (__atomic_load_n(&rv->state, __ATOMIC_ACQUIRE) == 2u) ? -2 : 0;
}

/* main = READER for a sibling writer; verify it drains `pushed`. */
static void main_reads(const char *name, Rdv *rv)
{
    int rc = wait_done(rv);
    if (rc == -1) { no(name, "sibling writer never finished"); return; }
    if (rc == -2) { no(name, "sibling writer open failed"); return; }

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
    volatile uint32_t wopened, ropened; /* 0 running, 1 ok, 2 fail                */
    volatile uint32_t pushed, popped;
    volatile uint32_t order_ok;         /* 1 iff every popped idx == expected      */
    volatile uint64_t done;             /* reader finished                         */
} Stress;

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
    if (!w) { __atomic_store_n(&s->wopened, 2u, __ATOMIC_RELEASE); for(;;) yield(); }
    __atomic_store_n(&s->wopened, 1u, __ATOMIC_RELEASE);
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
    /* reader may win the race and beat the writer's CREATE — bounded retry. */
    Brook *r = 0;
    for (int t = 0; t < 2000 && !r; t++) { r = brook_open(s->tag, FS, FC, BROOK_READER); if (!r) yield(); }
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
          int rc = wait_done(&rv);
          if (rc != 0) { no("T3", rc==-2 ? "sibling writer open failed (BUSY?)" : "writer timeout"); brook_release(r); }
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
        if (rc!=0) { no("T4", rc==-2?"sibling reader open failed":"reader timeout"); }
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
            /* Watchdog: a stuck peer FAILS the test, never hangs the suite. */
            uint32_t cyc = 0; int timedout = 0;
            while (__atomic_load_n(&s.done, __ATOMIC_ACQUIRE) == 0) {
                if (++cyc > 1200u) { timedout = 1; break; }         /* ~60s cap */
                addr_park(&s.done, 0, 50);
            }
            uint32_t wo = __atomic_load_n(&s.wopened, __ATOMIC_ACQUIRE);
            uint32_t ro = __atomic_load_n(&s.ropened, __ATOMIC_ACQUIRE);
            uint32_t pu = __atomic_load_n(&s.pushed,  __ATOMIC_ACQUIRE);
            uint32_t po = __atomic_load_n(&s.popped,  __ATOMIC_ACQUIRE);
            uint32_t od = __atomic_load_n(&s.order_ok, __ATOMIC_ACQUIRE);
            printf("[BS] T5 concurrent: wopened=%u ropened=%u pushed=%u popped=%u order_ok=%u timedout=%d (expect %u)\n",
                   wo, ro, pu, po, od, timedout, STRESS_FRAMES);
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
