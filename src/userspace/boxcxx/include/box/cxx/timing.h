#ifndef BOXCXX_BOX_TIMING_H
#define BOXCXX_BOX_TIMING_H

#include <chrono>

namespace box {

class stopwatch {
    std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};

public:
    stopwatch() noexcept = default;

    std::chrono::nanoseconds elapsed() const noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_);
    }
    template <class Duration>
    Duration elapsed_as() const noexcept
    {
        return std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now() - start_);
    }
    std::chrono::nanoseconds reset() noexcept
    {
        auto now = std::chrono::steady_clock::now();
        auto lap = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_);
        start_ = now;
        return lap;
    }
};

class throttle {
    std::chrono::steady_clock::duration   interval_;
    std::chrono::steady_clock::time_point last_{std::chrono::steady_clock::now()};
    bool                                  fired_{false};

public:
    explicit throttle(std::chrono::steady_clock::duration interval) noexcept : interval_(interval) {}

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
    bool ready() const noexcept
    {
        return !fired_ || (std::chrono::steady_clock::now() - last_) >= interval_;
    }
    std::chrono::nanoseconds remaining() const noexcept
    {
        if (ready()) return std::chrono::nanoseconds(0);
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            interval_ - (std::chrono::steady_clock::now() - last_));
    }
    void reset() noexcept { fired_ = false; }
};

}

#endif