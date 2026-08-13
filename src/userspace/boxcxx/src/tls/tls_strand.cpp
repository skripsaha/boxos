/*
 * tls_strand.cpp — per-strand local-exec TLS bootstrap for BoxOS C++ binaries
 * (Ф20b foundation).
 *
 * tls_init.cpp bootstraps the MAIN strand's C++ TLS from a priority-101
 * .init_array ctor. A strand spawned via strand_spawn runs NO .init_array, and
 * the kernel already pointed its fs-base at the per-strand StrandInfo (fs:0).
 * So the ONLY thing missing for that strand is the negative-offset
 * .tdata/.tbss image directly below fs:0 — which the kernel eager-maps as one
 * page at HAMMOCK_NEGTLS_PAGE (see strand_rings.c).
 *
 * __boxcxx_tls_strand_init casts that image from the same mold tls_init.cpp
 * uses (tls_mold.h — the layout contract lives there), the only difference
 * being the TCB: instead of a malloc'd 16-byte block it is the existing
 * StrandInfo, whose first 8 bytes are the variant-2 self-pointer at fs:0. It
 * does NOT set fs-base (the kernel did) and does NOT run .init_array.
 */

#include <cstdint>
#include <cstddef>

#include "box/types.h"             /* uint*_t for strand_info.h */
#include "strand_info.h"           /* StrandInfo, STRAND_INFO_MAGIC */
#include "box/cxx/tls_strand.h"

#include "tls_mold.h"

namespace boxcxx {
[[noreturn]] void Panic(const char *msg);
}

namespace {

/* The kernel eager-maps exactly ONE page for the neg-TLS block; a strand's TLS
 * image must fit it. VMM_PAGE_SIZE is a kernel-private constant, so the literal
 * is used here with this comment (the kernel side asserts the page geometry). */
constexpr uint64_t kNegTlsPageSize = 4096;

inline uint64_t ReadFsBase()
{
    uint64_t v;
    __asm__ volatile("rdfsbase %0" : "=r"(v));
    return v;
}

} // namespace

extern "C" void __boxcxx_tls_strand_init()
{
    namespace tls = boxcxx::__tls;

    /* fs:0 is the per-strand StrandInfo the kernel installed. Spawned strands
     * are gated on FSGSBASE (box/strand spawn refuses without it), so RDFSBASE
     * is always legal on this path. */
    const uint64_t fsbase = ReadFsBase();
    StrandInfo *si = reinterpret_cast<StrandInfo *>(static_cast<uintptr_t>(fsbase));
    if (si->magic != STRAND_INFO_MAGIC)
        boxcxx::Panic("strand TLS init: fs:0 is not a StrandInfo");

    const tls::Mold mold = tls::Take();
    if (mold.below_tcb > kNegTlsPageSize)
        boxcxx::Panic("strand neg-TLS exceeds one page");

    tls::Pour(reinterpret_cast<char *>(static_cast<uintptr_t>(fsbase) - mold.below_tcb),
              mold);

    /* fs-base, the TCB self-pointer (StrandInfo.tcb_self == fs:0) and the DTV
     * slot are already set by the kernel — nothing else to do. The strand's
     * first fs-relative access now resolves into this block. */
}

extern "C" void __boxcxx_thread_storage_enter()
{
    /* The thread_local destructor head (cxa_runtime.cpp) lives in this strand's
     * own neg-TLS, which the kernel zero-filled — so it is already empty. No
     * work today; the hook exists so std::thread (Ф20b-2) and any future
     * per-strand init have a single defined entry point. */
}
