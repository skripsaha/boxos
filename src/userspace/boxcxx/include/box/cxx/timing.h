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
//   box::pacer     — a steady frame rate: wait() says how long to sleep so each
//                    frame starts one interval after the previous one started.
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

// ── box::pacer — frames at a steady rate, each scheduled from the last start ──
class pacer {
    std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};

public:
    // The time one frame takes at `frames_per_second` (at least one).
    static std::chrono::microseconds interval(unsigned frames_per_second) noexcept
    {
        return std::chrono::microseconds(1000000 / (frames_per_second ? frames_per_second : 1));
    }
    // Call once a frame, after drawing it: how long to sleep before the next
    // frame starts, `interval` after this one started. A frame that overran
    // gets zero and the schedule begins again from now, so the frames that
    // were missed are not caught up in a burst.
    std::chrono::microseconds wait(std::chrono::microseconds interval) noexcept
    {
        const auto due = start_ + interval;
        const auto now = std::chrono::steady_clock::now();
        if (now >= due) {
            start_ = now;
            return std::chrono::microseconds(0);
        }
        start_ = due;
        return std::chrono::duration_cast<std::chrono::microseconds>(due - now);
    }
};

}  // namespace box

#endif  // BOXCXX_BOX_TIMING_H
