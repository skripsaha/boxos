
#include <__bits/par_engine>

#include <atomic>
#include <exception>
#include <mutex>
#include <new>
#include <thread>

#include "box/cxx/strand.h"

namespace std {
namespace __par {

namespace {

struct Worker {
    atomic<uint64_t> ticket{0};
    Chunk            fn    = nullptr;
    void            *ctx   = nullptr;
    size_t           idx   = 0;
    size_t           begin = 0;
    size_t           end   = 0;
    bool             quit  = false;
};

struct Brigade {
    mutex            lock;
    Worker          *workers = nullptr;
    box::strand     *strands = nullptr;
    size_t           count   = 0;
    atomic<uint64_t> done{0};
    bool             built   = false;
};

Brigade g_brigade;

thread_local bool t_inside = false;

void RunGuarded(Chunk fn, void *ctx, size_t idx, size_t begin, size_t end)
{
    try {
        fn(ctx, idx, begin, end);
    } catch (...) {
        std::terminate();
    }
}

void WorkerLoop(Worker *w)
{
    uint64_t seen = 0;
    for (;;) {
        uint64_t ticket = w->ticket.load(memory_order_acquire);
        while (ticket == seen) {
            box::park(w->ticket, seen);
            ticket = w->ticket.load(memory_order_acquire);
        }
        seen = ticket;
        if (w->quit) return;

        t_inside = true;
        RunGuarded(w->fn, w->ctx, w->idx, w->begin, w->end);
        t_inside = false;

        g_brigade.done.fetch_add(1, memory_order_release);
        box::wake(g_brigade.done);
    }
}

void Build()
{
    g_brigade.built = true;
    const unsigned hc = thread::hardware_concurrency();
    if (hc < 2) return;
    const size_t want = hc - 1;

    Worker *workers = new (nothrow) Worker[want];
    if (!workers) return;
    void *raw = ::operator new(sizeof(box::strand) * want, nothrow);
    if (!raw) {
        delete[] workers;
        return;
    }
    box::strand *strands = static_cast<box::strand *>(raw);

    size_t made = 0;
    for (; made < want; ++made) {
        try {
            new (&strands[made]) box::strand(WorkerLoop, &workers[made]);
        } catch (...) {
            break;
        }
    }
    if (made == 0) {
        ::operator delete(raw);
        delete[] workers;
        return;
    }
    g_brigade.workers = workers;
    g_brigade.strands = strands;
    g_brigade.count   = made;
}

struct Teardown {
    ~Teardown()
    {
        lock_guard<mutex> held(g_brigade.lock);
        const size_t n = g_brigade.count;
        g_brigade.count = 0;
        for (size_t i = 0; i < n; ++i) {
            g_brigade.workers[i].quit = true;
            g_brigade.workers[i].ticket.fetch_add(1, memory_order_release);
            box::wake(g_brigade.workers[i].ticket, 1);
        }
        for (size_t i = 0; i < n; ++i) g_brigade.strands[i].~strand();
        if (n) {
            ::operator delete(static_cast<void *>(g_brigade.strands));
            delete[] g_brigade.workers;
        }
        g_brigade.strands = nullptr;
        g_brigade.workers = nullptr;
    }
};
Teardown g_teardown;

}

size_t Width()
{
    if (t_inside) return 1;
    lock_guard<mutex> held(g_brigade.lock);
    if (!g_brigade.built) Build();
    return g_brigade.count + 1;
}

bool Nested() noexcept { return t_inside; }

struct Inside {
    Inside() { t_inside = true; }
    ~Inside() { t_inside = false; }
};

size_t Plan(size_t n, size_t grain)
{
    if (n == 0) return 0;
    if (t_inside || grain == 0) return 1;
    const size_t width = Width();
    size_t chunks = n / grain;
    if (chunks > width) chunks = width;
    return chunks < 2 ? 1 : chunks;
}

size_t RunExact(Chunk fn, void *ctx, size_t n, size_t chunks)
{
    if (n == 0) return 0;
    if (t_inside || chunks < 2) {
        RunGuarded(fn, ctx, 0, 0, n);
        return 1;
    }

    lock_guard<mutex> held(g_brigade.lock);
    if (chunks > g_brigade.count + 1) chunks = g_brigade.count + 1;
    if (chunks < 2) {
        Inside marked;
        RunGuarded(fn, ctx, 0, 0, n);
        return 1;
    }

    g_brigade.done.store(0, memory_order_relaxed);
    for (size_t i = 1; i < chunks; ++i) {
        Worker &w = g_brigade.workers[i - 1];
        w.fn    = fn;
        w.ctx   = ctx;
        w.idx   = i;
        w.begin = i * n / chunks;
        w.end   = (i + 1) * n / chunks;
        w.ticket.fetch_add(1, memory_order_release);
        box::wake(w.ticket, 1);
    }

    {
        Inside marked;
        RunGuarded(fn, ctx, 0, 0, n / chunks);
    }

    const uint64_t want = chunks - 1;
    for (;;) {
        const uint64_t got = g_brigade.done.load(memory_order_acquire);
        if (got == want) break;
        box::park(g_brigade.done, got);
    }
    return chunks;
}

size_t Run(Chunk fn, void *ctx, size_t n, size_t grain)
{
    return RunExact(fn, ctx, n, Plan(n, grain));
}

}
}