#ifndef BOXCXX_TLS_MOLD_H
#define BOXCXX_TLS_MOLD_H

/*
 * tls_mold.h — the one mold every C++ TLS block in this cabin is cast from.
 *
 * A cabin runs two kinds of strand, and each needs the same variant-2 TLS
 * image sitting below its fs-base:
 *
 *   main strand     — tls_init.cpp casts it on the heap from a priority-101
 *                     .init_array ctor, and points fs-base at a TCB of its own.
 *   spawned strand  — tls_strand.cpp casts it into the kernel's eager-mapped
 *                     neg-TLS page, below the StrandInfo the kernel already
 *                     installed as that strand's TCB.
 *
 * The two differ only in where the block lives and what sits at fs:0. Every
 * byte below the TCB — the .tdata copy, the .tbss zeros, the variant-2 padding
 * — is identical, and has to be: the compiler emits ONE negative offset per
 * thread_local, fixed at link time, and both casts must land that variable on
 * that one offset. Casting from a shared mold is what makes that structural
 * rather than a promise; the arithmetic below exists exactly once.
 *
 *      block:  [ .tdata copy | pad | .tbss zeros | pad ] [ TCB ]
 *                ^ tp - below_tcb                          ^ tp, and fs:0
 *      var offset = PT_TLS offset - below_tcb            (negative)
 *
 * The 64-byte figure is not a guess. tls_init.cpp's kAnchor is an alignas(64)
 * thread_local, which forces PT_TLS p_align == 64, which is in turn what makes
 * "the linker starts .tbss at AlignUp(tdata_size, 64)" true — so that anchor is
 * load-bearing for THIS arithmetic, not only for the ELF program header.
 * tls_init.cpp pins its alignment with a static_assert to keep it that way.
 */

#include <cstdint>
#include <cstddef>

#include "box/string.h"   /* memcpy, memset */

extern "C" {
/* user.ld — the .tdata load image and the two section sizes (symbol-value
 * trick: the "address" of a size symbol IS the size). Declared once, here, so
 * the two casts cannot fall out of step over these either. */
extern const char __tdata_start[];
extern const char __tdata_size[];
extern const char __tbss_size[];
}

namespace boxcxx::__tls {

/* Pinned by tls_init.cpp's kAnchor — see the banner. */
inline constexpr uint64_t kAlign = 64;

constexpr uint64_t AlignUp(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/* The shape of everything below the TCB. What sits AT the TCB is the caller's
 * business: a 16-byte block on the heap for the main strand, the kernel's
 * StrandInfo for a spawned one. */
struct Mold {
    uint64_t tdata_size;   /* bytes to copy out of the .tdata load image */
    uint64_t below_tcb;    /* block start to TCB, i.e. the thread pointer offset */
};

/* Read the shape off the linker symbols. */
inline Mold Take()
{
    const uint64_t tdata = (uint64_t)(uintptr_t)__tdata_size;
    const uint64_t tbss  = (uint64_t)(uintptr_t)__tbss_size;
    const uint64_t memsz = AlignUp(tdata, kAlign) + tbss;
    return Mold{tdata, AlignUp(memsz, kAlign)};
}

/* Pour the image into a block: the .tdata copy, then zeros across the .tbss
 * and the variant-2 padding, up to the TCB. `block` must be kAlign-aligned and
 * hold m.below_tcb bytes. */
inline void Pour(void *block, const Mold &m)
{
    memcpy(block, __tdata_start, m.tdata_size);
    memset(static_cast<char *>(block) + m.tdata_size, 0, m.below_tcb - m.tdata_size);
}

}  // namespace boxcxx::__tls

#endif /* BOXCXX_TLS_MOLD_H */
