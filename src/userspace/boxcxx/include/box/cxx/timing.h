// boxcxx — box::stopwatch / box::throttle  (timing helpers over std::chrono)
//
// Thin, idiomatic helpers on top of std::chrono::steady_clock (the monotonic
// boot-relative clock from <chrono>). NOT a raw-cycle counter — that is
// box::cpu::tsc_now() (box/cxx/cpu.h); these answer "how long did this take"
// and "may I act yet", not "what is the cycle count".
//
//   box::stopwatch — monotonic elapsed-time measurement: elapsed() / reset().
//   box::throttle  — a rate limiter: try_fire() succeeds at most once per
//                    interval (the first call fires).
//
// This is a box:: extension, not part of std. All time comes from
// std::chrono::steady_clock, so the resolution and monotonicity are exactly
// that clock's (validated in <chrono>); these types only wrap its arithmetic.
#ifndef BOXCXX_BOX_TIMING_H
#define BOXCXX_BOX_TIMING_H

#include <chrono>

namespace box {

// ── box::stopwatch — monotonic elapsed time since construction / last reset ──
class stopwatch {
    std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};

public:
    stopwatch() noexcept = default;

    // Time elapsed since construction or the last reset().
    std::chrono::nanoseconds elapsed() const noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_);
    }
    // elapsed() pre-cast to a chosen duration unit.
    template <class Duration>
    Duration elapsed_as() const noexcept
    {
        return std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now() - start_);
    }
    // Restart the window from now; returns the elapsed time just before the
    // reset (the lap), so `auto lap = sw.reset();` both reads and restarts.
    std::chrono::nanoseconds reset() noexcept
    {
        auto now = std::chrono::steady_clock::now();
        auto lap = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_);
        start_ = now;
        return lap;
    }
};

// ── box::throttle — rate limiter: at most one fire per interval ──────────────
class throttle {
    std::chrono::steady_clock::duration   interval_;
    std::chrono::steady_clock::time_point last_{std::chrono::steady_clock::now()};
    bool                                  fired_{false};

public:
    explicit throttle(std::chrono::steady_clock::duration interval) noexcept : interval_(interval) {}

    // True iff at least `interval` has elapsed since the last fire (the first
    // call always fires); when true it advances the window and consumes it.
    bool try_fire() noexcept
    {
        auto now = std::chrono::steady_clock::now();
        if (!fired_ || (now - last_) >= interval_) {
            last_  = now;
            fired_ = true;
            return true;
        }
        return false;
    }
    // Whether the next try_fire() would succeed — does NOT advance the window.
    bool ready() const noexcept
    {
        return !fired_ || (std::chrono::steady_clock::now() - last_) >= interval_;
    }
    // Time remaining until ready (zero if ready now).
    std::chrono::nanoseconds remaining() const noexcept
    {
        if (ready()) return std::chrono::nanoseconds(0);
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            interval_ - (std::chrono::steady_clock::now() - last_));
    }
    // Re-arm so the next try_fire() fires immediately.
    void reset() noexcept { fired_ = false; }
};

}  // namespace box

#endif  // BOXCXX_BOX_TIMING_H
