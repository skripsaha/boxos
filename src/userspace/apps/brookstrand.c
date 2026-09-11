
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

typedef struct {
    const char *tag;
    uint32_t    nframes;
    volatile uint64_t va_header;
    volatile uint32_t avail_after;
    volatile uint32_t pushed;
    volatile uint32_t popped;
    volatile uint64_t state;
} Rdv;

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
    for (;;) yield();
}

static void sib_reader(void *arg)
{
    Rdv *rv = (Rdv *)arg;
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

#define STRESS_FRAMES  10000u

typedef struct {
    const char       *tag;
    volatile uint32_t wready, rready;
    volatile uint64_t go;
    volatile uint64_t wopened;
    volatile uint32_t ropened;
    volatile uint32_t pushed, popped;
    volatile uint32_t order_ok;
    volatile uint64_t done;
} Stress;

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
    if (!w) { __atomic_store_n(&s->wopened, 2u, __ATOMIC_RELEASE);
              addr_wake(&s->wopened, 0); for(;;) yield(); }
    __atomic_store_n(&s->wopened, 1u, __ATOMIC_RELEASE);
    addr_wake(&s->wopened, 0);
    uint32_t n = 0;
    for (uint32_t i = 0; i < STRESS_FRAMES; i++) {
        uint8_t f[FS]; memset(f, 0, sizeof(f)); memcpy(f, &i, 4);
        if (brook_push_timeout(w, f, 5000) != 0) break;
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
    Brook *r = brook_open(s->tag, FS, FC, BROOK_READER);
    if (!r) {
        uint64_t wo;
        while ((wo = __atomic_load_n(&s->wopened, __ATOMIC_ACQUIRE)) == 0u) {
            error_t prc = addr_park(&s->wopened, 0u, 0);
            if (prc != OK && prc != ERR_ADDR_VALUE_MISMATCH)
                yield();
        }
        if (wo == 1u) r = brook_open(s->tag, FS, FC, BROOK_READER);
    }
    if (!r) { __atomic_store_n(&s->ropened, 2u, __ATOMIC_RELEASE);
              __atomic_store_n(&s->done, 1u, __ATOMIC_RELEASE); addr_wake(&s->done, 0); for(;;) yield(); }
    __atomic_store_n(&s->ropened, 1u, __ATOMIC_RELEASE);
    uint32_t got = 0, order = 1;
    for (uint32_t i = 0; i < STRESS_FRAMES; i++) {
        uint8_t f[FS];
        if (brook_pop_timeout(r, f, 5000) != 0) break;
        uint32_t idx = 0; memcpy(&idx, f, 4);
        if (idx != i) order = 0;
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

    { static Rdv rv; memset(&rv,0,sizeof rv); rv.tag="brook:strand:t1"; rv.nframes=8;
      if (strand_spawn(sib_writer, &rv)==0) no("T1","spawn"); else main_reads("T1", &rv); }

    { static Rdv rv; memset(&rv,0,sizeof rv); rv.tag="brook:strand:t2"; rv.nframes=1;
      if (strand_spawn(sib_writer, &rv)==0) no("T2","spawn"); else main_reads("T2", &rv); }

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

    {
        static Stress s; memset(&s, 0, sizeof s); s.tag = "brook:strand:t5";
        uint32_t wp = strand_spawn(stress_writer, &s);
        uint32_t rp = strand_spawn(stress_reader, &s);
        if (wp == 0 || rp == 0) { no("T5", "spawn"); }
        else {
            uint32_t bc = 0;
            while (!(__atomic_load_n(&s.wready, __ATOMIC_ACQUIRE) &&
                     __atomic_load_n(&s.rready, __ATOMIC_ACQUIRE))) {
                if (++bc > 4000u) break;
                yield();
            }
            __atomic_store_n(&s.go, 1u, __ATOMIC_RELEASE);
            uint32_t cyc = 0; int timedout = 0;
            while (__atomic_load_n(&s.done, __ATOMIC_ACQUIRE) == 0) {
                if (++cyc > 1200u) { timedout = 1; break; }
                addr_park(&s.done, 0, 50);
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