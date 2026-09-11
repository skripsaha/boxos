#ifndef BOXCXX_BOX_CPU_H
#define BOXCXX_BOX_CPU_H

#include <cstdint>
#include <limits>

#include "box/cpu.h"

namespace box {

namespace cpu {

inline bool has_waitpkg() noexcept       { return ::cpu_has_waitpkg(); }
inline bool has_invariant_tsc() noexcept { return ::cpu_has_invariant_tsc(); }
inline bool has_pku() noexcept           { return ::cpu_has_pku(); }
inline bool has_pks() noexcept           { return ::cpu_has_pks(); }
inline bool has_lam() noexcept           { return ::cpu_has_lam(); }
inline bool has_cet() noexcept           { return ::cpu_has_cet(); }
inline bool has_tme() noexcept           { return ::cpu_has_tme(); }
inline bool has_fsgsbase() noexcept      { return ::cpu_has_fsgsbase(); }
inline bool has_rdrand() noexcept        { return ::cpu_has_rdrand(); }
inline bool has_rdseed() noexcept        { return ::cpu_has_rdseed(); }

inline std::uint64_t tsc_freq_khz() noexcept              { return ::cpu_get_tsc_freq_khz(); }
inline std::uint64_t tsc_now() noexcept                   { return ::cpu_rdtsc(); }
inline std::uint64_t tsc_end() noexcept                   { return ::cpu_rdtsc_end(); }
inline std::uint64_t tsc_to_ns(std::uint64_t ticks) noexcept { return ::cpu_tsc_to_ns(ticks); }
inline std::uint64_t ms_to_tsc(std::uint64_t ms) noexcept    { return ::cpu_ms_to_tsc(ms); }

enum class wake {
    written,
    deadline,
    unavailable,
};

inline bool monitor_supported() noexcept { return ::cpu_has_waitpkg(); }

inline wake monitor_wait(const volatile void* addr, std::uint64_t deadline,
                         unsigned state = 0) noexcept
{
    if (!::cpu_has_waitpkg()) return wake::unavailable;
    ::umonitor(const_cast<volatile void*>(addr));
    return ::umwait(state, deadline) ? wake::deadline : wake::written;
}

}

class hardware_entropy {
public:
    using result_type = std::uint64_t;

    static constexpr result_type min() noexcept { return 0; }
    static constexpr result_type max() noexcept { return std::numeric_limits<result_type>::max(); }

    hardware_entropy() noexcept = default;
    hardware_entropy(const hardware_entropy&)            = delete;
    hardware_entropy& operator=(const hardware_entropy&) = delete;

    enum class source { seed, rand, none };
    static source active_source() noexcept
    {
        if (cpu::has_rdseed()) return source::seed;
        if (cpu::has_rdrand()) return source::rand;
        return source::none;
    }

    static bool engaged() noexcept { return active_source() != source::none; }

    double entropy() const noexcept { return engaged() ? 64.0 : 0.0; }

    result_type operator()() noexcept
    {
        std::uint64_t v;
        if (cpu::has_rdseed() && ::cpu_rdseed64(&v)) return v;
        if (cpu::has_rdrand() && ::cpu_rdrand64(&v)) return v;
        return tsc_mix();
    }

private:
    static result_type tsc_mix() noexcept
    {
        std::uint64_t z = ::cpu_rdtsc();
        z ^= z >> 30; z *= 0xbf58476d1ce4e5b9ull;
        z ^= z >> 27; z *= 0x94d049bb133111ebull;
        z ^= z >> 31;
        return z;
    }
};

}

#endif