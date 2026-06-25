// brookexec — cross-strand Brook + box::executor co_await regression test.
//
// A sibling strand is the Brook WRITER; the MAIN strand drives a box::executor
// draining a coroutine that does `co_await r->next()` (box::brook<T>, the Brook
// wait-domain awaiter). This is the precise C++ shape Ф24c (co_await current
// read, Brook-backed) builds on — it guards that an executor-driven cross-strand
// reader converges with a sibling writer. Proven on bios1/bios16/uefi1/uefi16.
//
// Emits [BE] PASS / [BE] FAIL.

#include <coroutine>
#include <cstdint>
#include <optional>

#include "box/cxx/executor.h"
#include "box/cxx/brook.h"

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

constexpr const char *TAG   = "brook:exec:repro";
constexpr std::uint32_t CAP = 16;
constexpr std::uint32_t N   = 8;

// sibling-strand rendezvous (shared cabin AS)
volatile std::uint64_t g_w_done;   // 0 running, 1 done, 2 open-fail
volatile std::uint32_t g_w_pushed;
volatile std::uint32_t g_w_avail;

// The sibling strand: open the Brook WRITER and push N ticks, then park alive.
extern "C" void writer_strand(void *)
{
    auto w = box::brook<Tick>::writer(TAG, CAP);
    if (!w) {
        __atomic_store_n(&g_w_done, 2u, __ATOMIC_RELEASE);
        addr_wake((void *)&g_w_done, 0);
        return;
    }
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < N; i++) {
        if (w.try_push(Tick{i, 0xBEEFu}) == 0) n++;
        else break;
    }
    __atomic_store_n(&g_w_pushed, n, __ATOMIC_RELAXED);
    __atomic_store_n(&g_w_avail, w.available(), __ATOMIC_RELAXED);
    __atomic_store_n(&g_w_done, 1u, __ATOMIC_RELEASE);
    addr_wake((void *)&g_w_done, 0);

    for (;;) yield();   // keep the writer + its mapping/alive flag live
}

// Main-strand consumer coroutine: drain `count` frames via co_await on the
// executor (the Brook wait-domain awaiter). Reports how many it actually got.
box::task<std::uint32_t> consumer(box::brook<Tick> *r, std::uint32_t count)
{
    std::uint32_t got = 0;
    for (std::uint32_t i = 0; i < count; i++) {
        std::optional<Tick> t = co_await r->next();
        if (!t) break;                 // stream terminal
        if (t->idx != i || t->mag != 0xBEEFu) break;
        got++;
    }
    co_return got;
}

} // namespace

int main()
{
    printf("[BE] brookexec start (executor + co_await brook_read, sibling writer)\n");

    if (!cpu_has_fsgsbase()) {
        printf("[BE] SKIP: strands require FSGSBASE (run under STRICT)\n");
        exit(0);
    }

    g_w_done = 0; g_w_pushed = 0; g_w_avail = 0;

    // Spawn the sibling WRITER strand first.
    std::uint32_t wpid = strand_spawn(writer_strand, nullptr);
    if (wpid == 0) { printf("[BE] FAIL: strand_spawn returned 0\n"); exit(1); }

    // Wait (bounded) for the writer to finish pushing.
    std::uint32_t cyc = 0;
    while (__atomic_load_n(&g_w_done, __ATOMIC_ACQUIRE) == 0) {
        if (++cyc > 400u) { printf("[BE] FAIL: writer never signalled done\n"); exit(1); }
        addr_park((void *)&g_w_done, 0, 50);
    }
    if (__atomic_load_n(&g_w_done, __ATOMIC_ACQUIRE) == 2u) {
        printf("[BE] FAIL: sibling writer open failed\n"); exit(1);
    }
    std::uint32_t pushed = __atomic_load_n(&g_w_pushed, __ATOMIC_RELAXED);
    std::uint32_t wav    = __atomic_load_n(&g_w_avail,  __ATOMIC_RELAXED);

    // Main strand attaches the READER and drains via the executor coroutine.
    auto r = box::brook<Tick>::reader(TAG);
    if (!r) { printf("[BE] FAIL: reader open NULL\n"); exit(1); }
    printf("[BE] writer pushed=%u wavail=%u | reader ravail=%u\n",
           pushed, wav, r.available());

    box::executor ex;
    std::uint32_t got = ex.block_on(consumer(&r, pushed));

    printf("[BE] executor drained=%u (expected=%u)\n", got, pushed);
    if (got == pushed && pushed == N) { printf("[BE] PASS\n"); exit(0); }
    printf("[BE] FAIL: executor reader did not receive the sibling writer frames\n");
    exit(1);
}
