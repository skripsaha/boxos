/*
 * helpers.cpp — the display's helpers. See helpers.h.
 *
 * A frame's tiles go out as a round: the work and how many jobs it has set,
 * then the round's number, which the helpers sleep on. Each comes in — counted
 * Inside, then sure the round is still the one it woke for — takes jobs one at
 * a time with the main strand until none is left, and goes out. The main
 * strand, its own jobs done, waits for the last ones taken; the round is then
 * unnamed, and before the next is set every helper still inside has gone out,
 * so none ever works a round gone by.
 */

#include "helpers.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>

#include "box/debug.h"
#include "box/strand.h"
#include "box/sync.h"
#include "box/system.h"
#include "frame.h"

namespace {

struct Round {
    alignas(8) std::atomic<std::uint64_t> Number{0};   // the round out, 0 for none: what the helpers sleep on
    std::atomic<std::uint32_t>                       Inside{0};
    std::atomic<std::size_t>                         Next{0}, Done{0};
    std::size_t                                      Jobs = 0;
    const std::function<void(std::size_t, std::size_t)> *Work = nullptr;
};
static_assert(sizeof(std::atomic<std::uint64_t>) == 8, "the word the helpers park on is 64 bits");

Round         g_round;
std::uint64_t g_rounds = 0;

/* Jobs of the round taken one at a time until none is left, by the strand
 * numbered Hand. */
void Take(std::size_t Hand) noexcept
{
    for (;;) {
        const std::size_t Job = g_round.Next.fetch_add(1);
        if (Job >= g_round.Jobs) return;
        (*g_round.Work)(Job, Hand);
        g_round.Done.fetch_add(1, std::memory_order_release);
    }
}

class Pool final : public Hands {
public:
    std::size_t Count() const noexcept override { return 1 + Made; }

    void Run(std::size_t Jobs, const std::function<void(std::size_t, std::size_t)> &Work) noexcept override
    {
        while (g_round.Inside.load() != 0) {
            /* A helper late from the round before, going out. */
        }
        g_round.Work = &Work;
        g_round.Jobs = Jobs;
        g_round.Next.store(0);
        g_round.Done.store(0);
        g_round.Number.store(++g_rounds);
        (void)addr_wake(&g_round.Number, 0);
        Take(0);
        while (g_round.Done.load(std::memory_order_acquire) < Jobs) {
            /* The helpers' last jobs, a tile's time at most. */
        }
        g_round.Number.store(0);
    }

    std::size_t Made = 0;
};

Pool g_pool;

/* A helper's life: asleep until a round it has not seen, then its part of it. */
void Serve(void *Numbered)
{
    const std::size_t Hand = reinterpret_cast<std::uintptr_t>(Numbered);
    std::uint64_t     Seen = 0;
    for (;;) {
        const std::uint64_t Out = g_round.Number.load();
        if (Out == 0 || Out == Seen) {
            (void)addr_park(&g_round.Number, Out, 0);
            continue;
        }
        Seen = Out;
        g_round.Inside.fetch_add(1);
        if (g_round.Number.load() == Out) Take(Hand);
        g_round.Inside.fetch_sub(1);
    }
}

}  // namespace

void HelpersStart() noexcept
{
    system_info_t       Info{};
    const std::uint32_t Cores  = sysinfo(&Info) == 0 ? Info.cpu_app_cores : 1u;
    const std::size_t   Wanted = std::min<std::size_t>(Cores > 1 ? Cores - 1 : 0, 3);
    for (std::size_t I = 0; I < Wanted; I++) {
        if (strand_spawn(Serve, reinterpret_cast<void *>(static_cast<std::uintptr_t>(I + 1))) == 0) break;
        g_pool.Made++;
    }
    if (g_pool.Made != 0) FrameUse(&g_pool);
    kdbg_print("[display] %u helper(s) compose the frames' tiles beside the display, of %u App Core(s)",
               static_cast<unsigned>(g_pool.Made), static_cast<unsigned>(Cores));
}
