
#include <cstdint>
#include <cstddef>

#include "box/types.h"
#include "strand_info.h"
#include "box/cxx/tls_strand.h"

#include "tls_mold.h"

namespace boxcxx {
[[noreturn]] void Panic(const char *msg);
}

namespace {

constexpr uint64_t kNegTlsPageSize = 4096;

inline uint64_t ReadFsBase()
{
    uint64_t v;
    __asm__ volatile("rdfsbase %0" : "=r"(v));
    return v;
}

}

extern "C" void __boxcxx_tls_strand_init()
{
    namespace tls = boxcxx::__tls;

    const uint64_t fsbase = ReadFsBase();
    StrandInfo *si = reinterpret_cast<StrandInfo *>(static_cast<uintptr_t>(fsbase));
    if (si->magic != STRAND_INFO_MAGIC)
        boxcxx::Panic("strand TLS init: fs:0 is not a StrandInfo");

    const tls::Mold mold = tls::Take();
    if (mold.below_tcb > kNegTlsPageSize)
        boxcxx::Panic("strand neg-TLS exceeds one page");

    tls::Pour(reinterpret_cast<char *>(static_cast<uintptr_t>(fsbase) - mold.below_tcb),
              mold);

}

extern "C" void __boxcxx_thread_storage_enter()
{
}