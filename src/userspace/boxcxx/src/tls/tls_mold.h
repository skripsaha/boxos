#ifndef BOXCXX_TLS_MOLD_H
#define BOXCXX_TLS_MOLD_H


#include <cstdint>
#include <cstddef>

#include "box/string.h"

extern "C" {
extern const char __tdata_start[];
extern const char __tdata_size[];
extern const char __tbss_size[];
}

namespace boxcxx::__tls {

inline constexpr uint64_t kAlign = 64;

constexpr uint64_t AlignUp(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

struct Mold {
    uint64_t tdata_size;
    uint64_t below_tcb;
};

inline Mold Take()
{
    const uint64_t tdata = (uint64_t)(uintptr_t)__tdata_size;
    const uint64_t tbss  = (uint64_t)(uintptr_t)__tbss_size;
    const uint64_t memsz = AlignUp(tdata, kAlign) + tbss;
    return Mold{tdata, AlignUp(memsz, kAlign)};
}

inline void Pour(void *block, const Mold &m)
{
    memcpy(block, __tdata_start, m.tdata_size);
    memset(static_cast<char *>(block) + m.tdata_size, 0, m.below_tcb - m.tdata_size);
}

}

#endif