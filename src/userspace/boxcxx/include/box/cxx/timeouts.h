// boxcxx — box::timeouts  (the chrono view of the BoxOS syscall timeouts)
//
// The idiomatic C++ face of box/timeouts.h. The C macros (BOX_TIMEOUT_*_MS)
// stay the single source of truth — these are exactly those values typed as
// std::chrono::milliseconds, so there is no second copy to drift, and the _MS
// suffix is dropped because the chrono type already carries the unit. A box::
// call that takes a std::chrono duration (box::after, the executor waits, …)
// can pass box::timeouts::storage instead of a bare 5000.
//
// This is a box:: extension, not part of std.
#ifndef BOXCXX_BOX_TIMEOUTS_H
#define BOXCXX_BOX_TIMEOUTS_H

#include <chrono>

#include "box/timeouts.h"  // BOX_TIMEOUT_*_MS — the single source of truth

namespace box {
namespace timeouts {

inline constexpr std::chrono::milliseconds fast    {BOX_TIMEOUT_FAST_MS};
inline constexpr std::chrono::milliseconds storage {BOX_TIMEOUT_STORAGE_MS};
inline constexpr std::chrono::milliseconds ipc     {BOX_TIMEOUT_IPC_MS};
inline constexpr std::chrono::milliseconds input   {BOX_TIMEOUT_INPUT_MS};
inline constexpr std::chrono::milliseconds kdbg    {BOX_TIMEOUT_KDBG_MS};

}  // namespace timeouts
}  // namespace box

#endif  // BOXCXX_BOX_TIMEOUTS_H
