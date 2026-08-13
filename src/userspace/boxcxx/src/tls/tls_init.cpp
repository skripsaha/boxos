/*
 * tls_init.cpp — local-exec TLS bootstrap for the MAIN strand of a BoxOS C++
 * binary.
 *
 * The kernel loader ignores PT_TLS; this runs as the very first .init_array
 * entry (constructor priority 101 — ahead of every default-priority global
 * constructor, which may itself touch thread_local). It casts the block from
 * the shared mold (tls_mold.h, which carries the layout contract), puts a TCB
 * of its own at the thread pointer and installs fs-base.
 *
 * Apps must not exceed alignas(64) on thread_local objects — the cxxtest.elf
 * link rule carries a readelf check for that.
 *
 * One C++ TLS block per cabin: this bootstraps the MAIN strand only. Its FS
 * base points at the C++ TCB here (on the heap). Strands spawned via
 * strand_spawn (P5a) instead get a kernel-populated StrandInfo TLS block in
 * the Hammock window as their FS base — boxlib (box/core/strand_self.c) tells
 * the two apart by the FS base's VA range, so this C++ TLS and per-strand
 * StrandInfo coexist on the same FS register without collision. The C++ TLS
 * block lives for the process lifetime; freed by the kernel with the heap.
 */

#include <cstdint>
#include <cstddef>

#include "box/cpu.h"
#include "box/system.h"

#include "tls_mold.h"

extern "C" {
void *_malloc_impl(size_t size);
}

namespace boxcxx {
[[noreturn]] void Panic(const char *msg);
}

namespace {

// Pins PT_TLS p_align to 64 for every C++ binary, which is what lets tls_mold.h
// place .tbss at AlignUp(tdata_size, 64). Lives in .tbss; one byte plus
// alignment padding per process.
[[gnu::used]] alignas(boxcxx::__tls::kAlign) thread_local char kAnchor;

// The anchor is load-bearing for the mold's arithmetic, not just for the ELF
// program header — losing the alignas would silently reshape every TLS block.
static_assert(__alignof__(kAnchor) == boxcxx::__tls::kAlign,
              "kAnchor must stay alignas(kAlign): it is what pins PT_TLS p_align");

struct TlsControlBlock {
    void *self;       // fs:0 — address-of-thread-local sequences load this
    void *reserved;   // keeps the TCB 16 bytes, matches SysV expectations
};

} // namespace

extern "C" void __boxcxx_tls_bootstrap()
{
    namespace tls = boxcxx::__tls;

    const tls::Mold mold = tls::Take();
    const uint64_t total = mold.below_tcb + sizeof(TlsControlBlock);

    void *raw = _malloc_impl(total + tls::kAlign);
    if (!raw) boxcxx::Panic("TLS bootstrap: heap exhausted");

    uintptr_t block = tls::AlignUp((uintptr_t)raw, tls::kAlign);

    tls::Pour((void *)block, mold);

    TlsControlBlock *tcb = (TlsControlBlock *)(block + mold.below_tcb);
    tcb->self     = tcb;
    tcb->reserved = nullptr;

    if (cpu_has_fsgsbase()) {
        __asm__ volatile("wrfsbase %0" :: "r"(tcb));
    } else {
        if (tls_set_fsbase((uint64_t)(uintptr_t)tcb) != 0)
            boxcxx::Panic("TLS bootstrap: SYSTEM_OP_TLS_FSBASE failed");
        /* The kernel installs the base at the next context restore —
         * force one so thread_local is live when we return. */
        yield();
    }

    /* First fs-relative access — faults here mean the base never landed. */
    kAnchor = 1;
}

/* Highest-priority constructor: runs before all default-priority global
 * ctors so they can safely construct thread_local state. */
[[gnu::constructor(101)]] static void BoxCxxTlsCtor()
{
    __boxcxx_tls_bootstrap();
}
