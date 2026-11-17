/*
 * cpu_path.cpp — which way the engine's rows are worked. See
 * painter/core/cpu_path.h.
 */

#include "painter/core/cpu_path.h"

#include <atomic>

#include "lanes.h"

namespace painter::core {

namespace {

#if defined(__x86_64__)
void Cpuid(std::uint32_t Leaf, std::uint32_t Sub, std::uint32_t Out[4]) noexcept
{
    asm volatile("cpuid" : "=a"(Out[0]), "=b"(Out[1]), "=c"(Out[2]), "=d"(Out[3]) : "a"(Leaf), "c"(Sub));
}

/* AVX2 on the processor, and the system keeping the YMM registers of every
 * strand (XCR0 bits 1 and 2) — else using them would lose them. */
bool HasAvx2() noexcept
{
    std::uint32_t R[4];
    Cpuid(0, 0, R);
    if (R[0] < 7u) return false;
    Cpuid(1, 0, R);
    const bool Kept = (R[2] >> 27) & 1u, Avx = (R[2] >> 28) & 1u;
    if (!Kept || !Avx) return false;
    std::uint32_t Low, High;
    asm volatile("xgetbv" : "=a"(Low), "=d"(High) : "c"(0u));
    if ((Low & 6u) != 6u) return false;
    Cpuid(7, 0, R);
    return (R[1] >> 5) & 1u;
}
#endif

std::atomic<CpuPath> &Way() noexcept
{
    static std::atomic<CpuPath> Chosen{BestCpuPath()};
    return Chosen;
}

}  // namespace

CpuPath BestCpuPath() noexcept
{
#if defined(__x86_64__)
    static const CpuPath Best = HasAvx2() ? CpuPath::Avx2 : CpuPath::Sse2;
    return Best;
#else
    return CpuPath::Plain;
#endif
}

CpuPath ChosenCpuPath() noexcept { return Way().load(std::memory_order_relaxed); }

bool UseCpuPath(CpuPath Path) noexcept
{
    if (static_cast<std::uint8_t>(Path) > static_cast<std::uint8_t>(BestCpuPath())) return false;
    Way().store(Path, std::memory_order_relaxed);
    return true;
}

const char *CpuPathName(CpuPath Path) noexcept
{
    switch (Path) {
    case CpuPath::Plain: return "plain";
    case CpuPath::Sse2: return "SSE2";
    case CpuPath::Avx2: return "AVX2";
    }
    return "?";
}

namespace lanes {

const Kit &Chosen() noexcept
{
    switch (ChosenCpuPath()) {
#if defined(__x86_64__)
    case CpuPath::Avx2: return Avx2();
    case CpuPath::Sse2: return Sse2();
#endif
    default: return Plain();
    }
}

}  // namespace lanes

}  // namespace painter::core
