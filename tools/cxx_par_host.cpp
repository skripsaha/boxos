// cxx_par_host.cpp — a brigade for the host stand.
//
// src/runtime/par_engine.cpp is a cabin's crew of strands and cannot be built
// by a host compiler. This file gives the same five entry points a host-side
// implementation over std::thread, so the third column of a stand exercises
// the SPLIT path rather than quietly running everything on one strand.
//
// ‼ The division is copied from the real engine on purpose, not approximated:
// chunk i covers [i*n/chunks, (i+1)*n/chunks) and the caller takes chunk 0.
// A stand that split the range differently would still agree on the answers
// and miss exactly the bugs that live on a boundary.
//
// Threads are spawned per region rather than parked in a pool. A pool is what
// the real engine needs and what a stand does not: the point here is that two
// chunks genuinely run at once, so ThreadSanitizer has something to watch.
#include <__bits/par_engine>

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace std {
namespace __par {

static thread_local bool t_inside = false;

size_t Width()
{
    unsigned hc = thread::hardware_concurrency();
    return hc ? hc : 1;
}

bool Nested() noexcept { return t_inside; }

size_t Plan(size_t n, size_t grain)
{
    if (t_inside || grain == 0) return 1;
    size_t width  = Width();
    size_t chunks = n / grain;
    if (chunks > width) chunks = width;
    return chunks < 2 ? 1 : chunks;
}

size_t RunExact(Chunk fn, void *ctx, size_t n, size_t chunks)
{
    if (t_inside || chunks < 2) {
        bool outer = !t_inside;
        t_inside = true;
        fn(ctx, 0, 0, n);
        if (outer) t_inside = false;
        return 1;
    }
    // ‼ BOXCXX_PAR_TRACE says how the work was actually divided. A stand that
    // cannot show whether it exercised the path it exists for is a stand that
    // agrees with the references for the wrong reason -- everything running on
    // one thread also agrees.
    if (getenv("BOXCXX_PAR_TRACE"))
        fprintf(stderr, "[par] n=%zu chunks=%zu\n", n, chunks);

    vector<thread> crew;
    crew.reserve(chunks - 1);
    for (size_t i = 1; i < chunks; ++i) {
        size_t b = i * n / chunks, e = (i + 1) * n / chunks;
        crew.emplace_back([fn, ctx, i, b, e] {
            t_inside = true;
            fn(ctx, i, b, e);
            t_inside = false;
        });
    }
    t_inside = true;
    fn(ctx, 0, 0, n / chunks);
    t_inside = false;
    for (thread &t : crew) t.join();
    return chunks;
}

size_t Run(Chunk fn, void *ctx, size_t n, size_t grain)
{
    return RunExact(fn, ctx, n, Plan(n, grain));
}

} // namespace __par
} // namespace std
