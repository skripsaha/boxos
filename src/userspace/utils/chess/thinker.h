/* thinker.h — the computer thinking on a strand of its own while the board
 * stays the person's: the keys are heard and the display goes on playing.
 * It answers by the moment its level gives it (Due): the game sets its alarm
 * for that moment and takes the move then, or at once if it came sooner. */
#ifndef CHESS_THINKER_H
#define CHESS_THINKER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "engine.h"
#include "position.h"

namespace chess {

class Thinker {
public:
    Thinker();
    ~Thinker();
    Thinker(const Thinker &)            = delete;
    Thinker &operator=(const Thinker &) = delete;

    /* Thinking begins on At: any thinking before is given up. */
    void Start(const Position &At, int Level, std::uint64_t Seed);
    /* Thinking, its answer not taken yet. */
    bool          Busy() const noexcept { return Running; }
    bool          Ready() const noexcept { return Running && Finished.load(std::memory_order_acquire); }
    std::uint64_t DueUs() const noexcept { return Due; }
    /* The answer, once Ready. */
    Move Take();
    /* The thinking given up, the strand waited for. */
    void Cancel();

private:
    std::unique_ptr<Engine> Brain;
    std::thread             Strand;
    std::atomic<bool>       Stop{false}, Finished{false};
    Move                    Answer;
    std::uint64_t           Due     = 0;
    bool                    Running = false;
};

}  // namespace chess

#endif
