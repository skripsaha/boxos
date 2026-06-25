// currentexec — cross-strand box::current<T> + box::executor co_await proof.
//
// A sibling strand is the Current stream WRITER; the MAIN strand drives a
// box::executor draining a coroutine that does `co_await r->next()` (the Ф24c
// Current read awaiter, Brook-backed). It proves an executor-driven reader
// converges with a sibling writer over the Current I/O spine, AND that the
// honest writer-leave terminal (w.close()) surfaces as a nullopt to the
// coroutine reader. Modeled on brookexec, swapping box::brook<Tick> ->
// box::current<Tick>, with two deliberate differences:
//   * a reader-attached rendezvous before the writer's close() — the kernel
//     BrookObject is ref-counted, so closing before the reader attaches would
//     free the stream under the racing open (brookexec never closed).
//   The writer parks (addr_park) on the attach flag — event-driven, and it works
//   fine from a spawned strand (proven by strandpark + the concurrent-park stress).
//
// (The executor's block-then-repoll latch — the path a concurrent producer hits
// when a frame arrives DURING the reader's native block — is proven
// deterministically by cxxtest Phase50, not here. This app guards the
// cross-strand convergence + the honest writer-leave terminal.)
//
// Emits [CE] PASS / [CE] FAIL.

#include <coroutine>
#include <cstdint>
#include <optional>

#include "box/cxx/executor.h"
#include "box/cxx/current.h"

extern "C" {
#include "box/print.h"
#include "box/system.h"
#include "box/sync.h"
#include "box/strand.h"
#include "box/cpu.h"
#include "box/error.h"
}

namespace {

struct Tick { std::uint32_t idx; std::uint32_t mag; };

constexpr const char *TAG = "current:exec:repro";
constexpr std::uint32_t N  = 8;

// sibling-strand rendezvous (shared cabin AS)
volatile std::uint64_t g_w_done;      // 0 running, 1 done, 2 open-fail
volatile std::uint32_t g_w_pushed;
volatile std::uint64_t g_r_attached;  // 1 once the reader has attached → writer may close

extern "C" void writer_strand(void *)
{
    auto w = box::current<Tick>(TAG, box::role::write);  // writer auto-creates
    if (!w) {
        __atomic_store_n(&g_w_done, 2u, __ATOMIC_RELEASE);
        addr_wake((void *)&g_w_done, 0);
        return;
    }
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < N; i++) {
        if (w.put(Tick{i, 0xBEEFu}).has_value()) n++;
        else break;
    }
    __atomic_store_n(&g_w_pushed, n, __ATOMIC_RELAXED);
    __atomic_store_n(&g_w_done, 1u, __ATOMIC_RELEASE);
    addr_wake((void *)&g_w_done, 0);

    // Hold the stream open until the reader has ATTACHED (ref-counted object —
    // closing before attach frees it under the racing open), then close: the
    // reader drains the buffered frames and observes the writer-leave terminal.
    // Park on the attach flag (event-driven, the BoxOS way) — main sets it +
    // addr_wake; the 50ms timeout re-checks; bounded so a missed wake can't hang.
    std::uint32_t wc = 0;
    while (__atomic_load_n(&g_r_attached, __ATOMIC_ACQUIRE) == 0) {
        if (++wc > 400u) break;
        addr_park((void *)&g_r_attached, 0, 50);
    }
    w.close();
    for (;;) yield();   // keep the writer's mapping/alive flag live
}

box::task<std::uint32_t> consumer(box::current<Tick> *r, std::uint32_t count,
                                  volatile bool *term_ok)
{
    std::uint32_t got = 0;
    for (std::uint32_t i = 0; i < count; i++) {
        std::optional<Tick> t = co_await r->next();
        if (!t) break;                          // unexpected early terminal
        if (t->idx != i || t->mag != 0xBEEFu) break;
        got++;
    }
    // Terminal proof: after the writer's frames + its close(), the next read is
    // the honest end-of-stream — a nullopt, never a Unix EOF.
    std::optional<Tick> after = co_await r->next();
    *term_ok = !after;
    co_return got;
}

} // namespace

int main()
{
    printf("[CE] currentexec start (executor + co_await current.next, sibling writer)\n");

    if (!cpu_has_fsgsbase()) {
        printf("[CE] SKIP: strands require FSGSBASE (run under STRICT)\n");
        exit(0);
    }

    g_w_done = 0; g_w_pushed = 0; g_r_attached = 0;

    std::uint32_t wpid = strand_spawn(writer_strand, nullptr);
    if (wpid == 0) { printf("[CE] FAIL: strand_spawn returned 0\n"); exit(1); }

    std::uint32_t cyc = 0;
    while (__atomic_load_n(&g_w_done, __ATOMIC_ACQUIRE) == 0) {
        if (++cyc > 400u) { printf("[CE] FAIL: writer never signalled done\n"); exit(1); }
        addr_park((void *)&g_w_done, 0, 50);
    }
    if (__atomic_load_n(&g_w_done, __ATOMIC_ACQUIRE) == 2u) {
        printf("[CE] FAIL: sibling writer open failed\n"); exit(1);
    }
    std::uint32_t pushed = __atomic_load_n(&g_w_pushed, __ATOMIC_RELAXED);

    auto r = box::current<Tick>(TAG, box::role::read);
    if (!r) { printf("[CE] FAIL: reader open NULL\n"); exit(1); }
    // The reader holds a ref now — release the writer to close (race-free terminal).
    __atomic_store_n(&g_r_attached, 1u, __ATOMIC_RELEASE);
    addr_wake((void *)&g_r_attached, 0);
    printf("[CE] writer pushed=%u\n", pushed);

    box::executor ex;
    volatile bool term_ok = false;
    std::uint32_t got = ex.block_on(consumer(&r, pushed, &term_ok));

    printf("[CE] executor drained=%u (expected=%u) terminal_nullopt=%d\n",
           got, pushed, (int)term_ok);
    if (got == pushed && pushed == N && term_ok) { printf("[CE] PASS\n"); exit(0); }
    printf("[CE] FAIL: convergence or terminal not observed\n");
    exit(1);
}
