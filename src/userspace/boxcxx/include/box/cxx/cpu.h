// boxcxx — box::cpu  (CPU feature gates, TSC timing, hardware entropy, monitor-wait)
//
// The idiomatic C++ surface over the boxlib CPU API (box/cpu.h), which reads
// the kernel-published, AP-intersected cpu_caps page (no syscall, no inline
// CPUID, no AP-divergence surprises):
//
//   box::cpu              — feature gates (has_waitpkg/_pku/_lam/_cet/_tme/…)
//                           + TSC timing (tsc_freq_khz / tsc_now / tsc_to_ns)
//                           + monitor_wait() — the WAITPKG user-mode wait.
//   box::hardware_entropy — a std uniform_random_bit_generator over on-chip
//                           entropy (RDSEED-preferred, RDRAND fallback). Plug
//                           it straight into seed_seq, an engine, or any
//                           <random> distribution.
//
// This is a box:: extension, not part of std. Everything DISENGAGES cleanly
// when the underlying ISA feature is absent: gates read false, monitor_wait()
// reports wake::unavailable without waiting, and hardware_entropy reports
// engaged()==false (degrading to a non-cryptographic TSC mix so the generator
// stays a total function).
#ifndef BOXCXX_BOX_CPU_H
#define BOXCXX_BOX_CPU_H

#include <cstdint>
#include <limits>

#include "box/cpu.h"  // cpu_has_* / cpu_get_tsc_freq_khz / cpu_rdtsc[_end] /
                      // cpu_tsc_to_ns / cpu_ms_to_tsc / cpu_rdseed64 /
                      // cpu_rdrand64 / umonitor / umwait

namespace box {

namespace cpu {

// ── feature gates (mirror the kernel-published cpu_caps page) ───────────────
// Each is true only when the feature survived every AP intersect, so the
// corresponding ISA use is safe on whatever core the cabin is scheduled on.
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

// ── TSC timing (benchmarking, not wall-clock — see <chrono> for wall-clock) ─
// tsc_now() is lfence;rdtsc (window start); tsc_end() is rdtsc;lfence (window
// end). tsc_to_ns() is meaningful only under has_invariant_tsc(); it returns 0
// when no calibrated frequency is available.
inline std::uint64_t tsc_freq_khz() noexcept              { return ::cpu_get_tsc_freq_khz(); }
inline std::uint64_t tsc_now() noexcept                   { return ::cpu_rdtsc(); }
inline std::uint64_t tsc_end() noexcept                   { return ::cpu_rdtsc_end(); }
inline std::uint64_t tsc_to_ns(std::uint64_t ticks) noexcept { return ::cpu_tsc_to_ns(ticks); }
inline std::uint64_t ms_to_tsc(std::uint64_t ms) noexcept    { return ::cpu_ms_to_tsc(ms); }

// ── WAITPKG monitor-wait (Intel SDM Vol 2A UMONITOR/UMWAIT) ─────────────────
// Why this wakeup landed — re-check your predicate on every wake.
enum class wake {
    written,      // a store hit the monitored line, or an interrupt arrived
    deadline,     // the TSC deadline / OS UMWAIT time-limit elapsed first
    unavailable,  // WAITPKG is absent — NO wait occurred; spin/yield yourself
};

inline bool monitor_supported() noexcept { return ::cpu_has_waitpkg(); }

// Arm a hardware monitor on *addr's cacheline, then halt the core in a low
// power state until a store lands on that line, an interrupt arrives, or the
// absolute TSC `deadline` passes. state 0 ⇒ C0.2 (deeper savings, slower
// wake), 1 ⇒ C0.1. Build the deadline from tsc_now() + ms_to_tsc(...).
//
// A store that lands in the gap between the caller's last predicate-check and
// the arm is bounded-stale: it is not lost forever, only delayed until the
// deadline — so callers loop {check → monitor_wait(deadline) → re-check}. This
// is the same bounded-staleness contract Brook and the <atomic> wait path use.
//
// Returns wake::unavailable WITHOUT waiting when the CPU lacks WAITPKG, so a
// caller can fall back to a spin/yield without a separate capability check.
inline wake monitor_wait(const volatile void* addr, std::uint64_t deadline,
                         unsigned state = 0) noexcept
{
    if (!::cpu_has_waitpkg()) return wake::unavailable;
    ::umonitor(const_cast<volatile void*>(addr));
    return ::umwait(state, deadline) ? wake::deadline : wake::written;
}

}  // namespace cpu

// ── box::hardware_entropy — a std uniform_random_bit_generator ──────────────
// On-chip entropy as a first-class generator: prefers RDSEED (seed-grade, the
// NIST SP 800-90B entropy source) and falls back to RDRAND (the SP 800-90A
// DRBG). 64-bit native (vs std::random_device's 32-bit), so a single draw
// fills a 64-bit engine seed. Models the uniform_random_bit_generator
// requirements (result_type / static min()/max() / operator()), so it drops
// straight into std::seed_seq, any engine's seed(), or a distribution.
//
// On a CPU exposing neither RDSEED nor RDRAND it degrades to a non-cryptographic
// TSC mix: operator() stays a total function, but engaged() reports false and
// entropy() reports 0 — never silently claim entropy you don't have.
class hardware_entropy {
public:
    using result_type = std::uint64_t;

    static constexpr result_type min() noexcept { return 0; }
    static constexpr result_type max() noexcept { return std::numeric_limits<result_type>::max(); }

    hardware_entropy() noexcept = default;
    hardware_entropy(const hardware_entropy&)            = delete;
    hardware_entropy& operator=(const hardware_entropy&) = delete;

    // Which on-chip source backs operator() right now.
    enum class source { seed, rand, none };
    static source active_source() noexcept
    {
        if (cpu::has_rdseed()) return source::seed;
        if (cpu::has_rdrand()) return source::rand;
        return source::none;
    }

    // True iff a real hardware entropy source (RDSEED or RDRAND) backs draws.
    static bool engaged() noexcept { return active_source() != source::none; }

    // Shannon-entropy estimate per draw, in bits: the full 64-bit width from a
    // hardware source, 0 in the degraded TSC mode (matches the width-reporting
    // convention of std::random_device::entropy()).
    double entropy() const noexcept { return engaged() ? 64.0 : 0.0; }

    result_type operator()() noexcept
    {
        std::uint64_t v;
        if (cpu::has_rdseed() && ::cpu_rdseed64(&v)) return v;
        if (cpu::has_rdrand() && ::cpu_rdrand64(&v)) return v;
        return tsc_mix();  // last resort: total but non-cryptographic
    }

private:
    // splitmix64 over a single RDTSC read — the same finalizer std::random_device
    // uses for its fallback, widened to 64 bits.
    static result_type tsc_mix() noexcept
    {
        std::uint64_t z = ::cpu_rdtsc();
        z ^= z >> 30; z *= 0xbf58476d1ce4e5b9ull;
        z ^= z >> 27; z *= 0x94d049bb133111ebull;
        z ^= z >> 31;
        return z;
    }
};

}  // namespace box

#endif  // BOXCXX_BOX_CPU_H
