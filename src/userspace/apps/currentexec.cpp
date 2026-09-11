
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

volatile std::uint64_t g_w_done;
volatile std::uint32_t g_w_pushed;
volatile std::uint64_t g_r_attached;

extern "C" void writer_strand(void *)
{
    auto w = box::current<Tick>(TAG, box::role::write);
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

    std::uint32_t wc = 0;
    while (__atomic_load_n(&g_r_attached, __ATOMIC_ACQUIRE) == 0) {
        if (++wc > 400u) break;
        addr_park((void *)&g_r_attached, 0, 50);
    }
    w.close();
    for (;;) yield();
}

box::task<std::uint32_t> consumer(box::current<Tick> *r, std::uint32_t count,
                                  volatile bool *term_ok)
{
    std::uint32_t got = 0;
    for (std::uint32_t i = 0; i < count; i++) {
        std::optional<Tick> t = co_await r->next();
        if (!t) break;
        if (t->idx != i || t->mag != 0xBEEFu) break;
        got++;
    }
    std::optional<Tick> after = co_await r->next();
    *term_ok = !after;
    co_return got;
}

}

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