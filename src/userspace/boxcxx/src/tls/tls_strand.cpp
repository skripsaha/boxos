/*
 * tls_strand.cpp — per-strand local-exec TLS bootstrap for BoxOS C++ binaries
 * (Ф20b foundation).
 *
 * tls_init.cpp bootstraps the MAIN strand's C++ TLS from a priority-101
 * .init_array ctor: it malloc's a block, copies .tdata, zeroes .tbss, and
 * points fs-base at a fresh TCB. A strand spawned via strand_spawn runs NO
 * .init_array, and the kernel already pointed its fs-base at the per-strand
 * StrandInfo (fs:0). So the ONLY thing missing for that strand is the
 * negative-offset .tdata/.tbss image directly below fs:0 — which the kernel
 * eager-maps as one page at HAMMOCK_NEGTLS_PAGE (see strand_rings.c).
 *
 * __boxcxx_tls_strand_init populates that page with the SAME byte layout
 * tls_init.cpp produces (x86-64 TLS variant 2), the only difference being the
 * TCB: instead of a malloc'd 16-byte block it is the existing StrandInfo (whose
 * first 8 bytes are the variant-2 self-pointer at fs:0). It does NOT set
 * fs-base (the kernel did) and does NOT run .init_array.
 *
 *      block:  [ .tdata copy | pad | .tbss zeros | pad ] [ StrandInfo (TCB) ]
 *                ^ fsbase - tp_off                        ^ fsbase (fs:0)
 *      tp_off = AlignUp(AlignUp(tdata,64) + tbss, 64)   — tls_init.cpp's formula
 *      var offset = PT_TLS offset − tp_off              (negative; identical)
 */

#include <cstdint>
#include <cstddef>

#include "box/string.h"            /* memcpy, memset */
#include "box/types.h"             /* uint*_t for strand_info.h */
#include "strand_info.h"           /* StrandInfo, STRAND_INFO_MAGIC */
#include "box/cxx/tls_strand.h"

extern "C" {
/* user.ld — .tdata load image and section sizes (symbol-value trick: the
 * "address" of the size symbols IS the size). Declared exactly as tls_init.cpp,
 * so both strands compute an identical layout from the same linker symbols. */
extern const char __tdata_start[];
extern const char __tdata_size[];
extern const char __tbss_size[];
}

namespace boxcxx {
[[noreturn]] void Panic(const char *msg);
}

namespace {

constexpr uint64_t AlignUp(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

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
    /* fs:0 is the per-strand StrandInfo the kernel installed. Spawned strands
     * are gated on FSGSBASE (box/strand spawn refuses without it), so RDFSBASE
     * is always legal on this path. */
    const uint64_t fsbase = ReadFsBase();
    StrandInfo *si = reinterpret_cast<StrandInfo *>(static_cast<uintptr_t>(fsbase));
    if (si->magic != STRAND_INFO_MAGIC)
        boxcxx::Panic("strand TLS init: fs:0 is not a StrandInfo");

    const uint64_t tdata_size = (uint64_t)(uintptr_t)__tdata_size;
    const uint64_t tbss_size  = (uint64_t)(uintptr_t)__tbss_size;
    constexpr uint64_t kAlign = 64;   /* pinned by tls_init.cpp's kAnchor */

    const uint64_t tbss_off = AlignUp(tdata_size, kAlign);
    const uint64_t memsz    = tbss_off + tbss_size;
    const uint64_t tp_off   = AlignUp(memsz, kAlign);

    if (tp_off > kNegTlsPageSize)
        boxcxx::Panic("strand neg-TLS exceeds one page");

    char *block = reinterpret_cast<char *>(static_cast<uintptr_t>(fsbase) - tp_off);

    /* Identical to tls_init.cpp: copy the .tdata image, zero the rest of the
     * block up to the TCB (.tbss + variant-2 alignment padding). */
    memcpy(block, __tdata_start, tdata_size);
    memset(block + tdata_size, 0, tp_off - tdata_size);

    /* fs-base, the TCB self-pointer (StrandInfo.tcb_self == fs:0) and the DTV
     * slot are already set by the kernel — nothing else to do. The strand's
     * first fs-relative thread_local access now resolves into this block. */
}

extern "C" void __boxcxx_thread_storage_enter()
{
    /* The thread_local destructor head (cxa_runtime.cpp) lives in this strand's
     * own neg-TLS, which the kernel zero-filled — so it is already empty. No
     * work today; the hook exists so std::thread (Ф20b-2) and any future
     * per-strand init have a single defined entry point. */
}
