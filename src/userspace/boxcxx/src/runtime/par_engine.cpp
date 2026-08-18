/*
 * par_engine.cpp — the brigade std::execution::par runs on.
 *
 * The decision this file implements: a parallel algorithm does NOT hire
 * strands and let them go. A cabin keeps one brigade, built the first time
 * anything asks for parallelism, asleep on the kernel's address-park until
 * there is work, and reused by every call after that. The alternative --
 * spawning per call -- makes par SLOWER than seq on anything short, which is
 * the opposite of what the policy is for; and polling instead of parking
 * would burn the very cores the work needs.
 *
 * One brigade, one job at a time. A second strand asking for parallelism
 * while a region is running waits for it rather than splitting the crew,
 * which keeps the dispatch free of per-call allocation. Nested parallelism --
 * a parallel algorithm called from inside one -- runs sequentially, which the
 * standard permits (an execution policy is permission, not obligation) and
 * which is the only way to be sure the brigade cannot wait on itself.
 *
 * Exceptions: [algorithms.parallel.exceptions] says an element access
 * function that exits by throwing calls terminate(). That is not a shortcut
 * here, it is the specified behaviour, and it is why the worker body catches
 * everything and calls terminate itself rather than letting an exception walk
 * out of a strand entry point.
 *
 * Without strands (no FSGSBASE, so strand_spawn refuses) the brigade has zero
 * workers and every parallel call runs on the caller. The policies still
 * mean what they say -- seq, par, unseq and par_unseq are all permission to
 * execute sequentially, and that is what a machine with one usable core does.
 */

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
    atomic<uint64_t> ticket{0};        // bumped to hand out a job; parked on
    Chunk            fn    = nullptr;
    void            *ctx   = nullptr;
    size_t           idx   = 0;
    size_t           begin = 0;
    size_t           end   = 0;
    bool             quit  = false;
};

struct Brigade {
    mutex            lock;             // creation, and one region at a time
    Worker          *workers = nullptr;
    box::strand     *strands = nullptr;
    size_t           count   = 0;
    atomic<uint64_t> done{0};
    bool             built   = false;
};

Brigade g_brigade;

// Set on a strand while it is inside a parallel region, so a nested call
// takes the sequential path instead of waiting for a brigade that is already
// busy with the outer one.
thread_local bool t_inside = false;

void RunGuarded(Chunk fn, void *ctx, size_t idx, size_t begin, size_t end)
{
    try {
        fn(ctx, idx, begin, end);
    } catch (...) {
        std::terminate();              // [algorithms.parallel.exceptions]
    }
}

void WorkerLoop(Worker *w)
{
    uint64_t seen = 0;
    for (;;) {
        uint64_t ticket = w->ticket.load(memory_order_acquire);
        while (ticket == seen) {
            box::park(w->ticket, seen);    // no lost wake: the kernel re-checks
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

// Called under g_brigade.lock.
void Build()
{
    g_brigade.built = true;
    const unsigned hc = thread::hardware_concurrency();
    if (hc < 2) return;                    // nothing to share the work with
    const size_t want = hc - 1;            // the caller takes a chunk too

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
            break;                         // no strands available: stay smaller
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

// Winds the brigade down before the cabin does. Workers park forever by
// design, so they have to be told; count is cleared first, so a parallel call
// arriving during teardown simply runs on its own caller.
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

} // namespace

size_t Width()
{
    // Asking from INSIDE a region must not take the lock: the region already
    // holds it, and a reduction sizes its partials array before it knows
    // whether it is nested. A nested region is one chunk wide anyway.
    if (t_inside) return 1;
    lock_guard<mutex> held(g_brigade.lock);
    if (!g_brigade.built) Build();
    return g_brigade.count + 1;
}

bool Nested() noexcept { return t_inside; }

// Marks the calling strand as being inside a region for as long as it lives.
// Anything the body then calls takes the sequential path, which is the only
// path that does not want the brigade lock -- and the lock is held for the
// whole of a real region.
struct Inside {
    Inside() { t_inside = true; }
    ~Inside() { t_inside = false; }
};

// Deciding whether the range is worth splitting happens WITHOUT the lock, and
// so does running it when it is not. The first version of Run did the
// short-range case under the lock, and a body that then called another
// parallel algorithm blocked on a mutex its own caller was holding -- a
// self-deadlock that only appears when the outer range is too short to split
// and the inner one is not. Found by phase201.
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
    if (chunks < 2) {                          // the brigade went away (teardown)
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
        RunGuarded(fn, ctx, 0, 0, n / chunks);    // the caller works too
    }

    const uint64_t want = chunks - 1;
    for (;;) {
        const uint64_t got = g_brigade.done.load(memory_order_acquire);
        if (got == want) break;
        box::park(g_brigade.done, got);    // value_mismatch if it already moved
    }
    return chunks;
}

size_t Run(Chunk fn, void *ctx, size_t n, size_t grain)
{
    return RunExact(fn, ctx, n, Plan(n, grain));
}

} // namespace __par
} // namespace std
