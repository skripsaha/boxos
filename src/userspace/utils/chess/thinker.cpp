/* thinker.cpp — the engine on its own strand. See thinker.h. */

#include "thinker.h"

#include "box/clock.h"

namespace chess {

Thinker::Thinker() : Brain(std::make_unique<Engine>()) {}

Thinker::~Thinker()
{
    Cancel();
}

void Thinker::Start(const Position &At, int Level, std::uint64_t Seed)
{
    Cancel();
    const Limits Of = LimitsOf(Level, clock_uptime_us(), Seed);
    Due             = Of.DueUs;
    Stop.store(false);
    Finished.store(false);
    Running = true;
    Strand  = std::thread([this, At, Of] {
        Answer = Brain->Think(At, Of, Stop);
        Finished.store(true, std::memory_order_release);
    });
}

Move Thinker::Take()
{
    if (Strand.joinable()) Strand.join();
    Running = false;
    return Answer;
}

void Thinker::Cancel()
{
    if (!Strand.joinable()) return;
    Stop.store(true);
    Strand.join();
    Running = false;
}

}  // namespace chess
