#ifndef PAINTER_CORE_CPU_PATH_H
#define PAINTER_CORE_CPU_PATH_H

#include <cstdint>

namespace painter::core {

// The ways the engine's rows of pixels are worked: plain, with SSE2 (every
// x86-64), with AVX2 (Haswell and later). Each gives the very same bits; the
// fastest the processor offers is chosen as the engine first works.
enum class CpuPath : std::uint8_t { Plain, Sse2, Avx2 };

// The fastest way this processor offers — AVX2 only where the system keeps
// its registers too (XCR0).
CpuPath BestCpuPath() noexcept;
// The way in use: the best, unless told otherwise.
CpuPath ChosenCpuPath() noexcept;
// Every canvas worked Path's way from now on — for the checks that hold the
// ways to one another; false, and nothing changed, when the processor has
// not it.
bool UseCpuPath(CpuPath Path) noexcept;
const char *CpuPathName(CpuPath Path) noexcept;

}  // namespace painter::core

#endif  // PAINTER_CORE_CPU_PATH_H
