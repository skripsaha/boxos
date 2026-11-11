/* history.h — the load over time: the whole machine's and each core's, from
 * the difference of two readings, kept as far back as the person set and
 * given back as the columns of a graph of any width. Nothing of the screen. */
#ifndef TASKBOARD_HISTORY_H
#define TASKBOARD_HISTORY_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "machine.h"

namespace taskboard {

class History {
public:
    // The reading's load added, and what is older than KeepSeconds let go.
    void Take(const Reading &Now, std::uint32_t KeepSeconds);

    // The latest load, 0 to 1: the machine's, and a core's — none for a core
    // whose counters did not move (a core the scheduler does not count).
    double                Machine() const noexcept;
    std::optional<double> Core(std::size_t Index) const noexcept;

    // Count columns over the last Seconds, oldest first: each the mean load
    // of the readings in its span — the machine's, or Core's — held from the
    // reading before when its span saw none, and none before the first.
    std::vector<std::optional<float>> Columns(std::optional<std::size_t> Core, std::size_t Count,
                                              std::uint32_t Seconds) const;

private:
    struct Sample {
        std::uint64_t      AtUs = 0;
        float              Machine = 0;
        std::vector<std::optional<float>> Cores;
    };

    std::vector<CoreSeen> Before;   // the counters of the reading before
    std::deque<Sample>    Samples;
};

}  // namespace taskboard

#endif
